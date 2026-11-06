#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../src/network/LocalSendJson.h"
#include "../../src/network/LocalSendSession.h"
using namespace localsend;
namespace {
int checks = 0;
char id[33], token[33];
void require(bool ok, const char* text) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", text);
    std::exit(1);
  }
}
std::string offer(const char* name = "Book.epub", uint64_t size = 1378) {
  JsonDocument doc;
  doc["info"]["alias"] = "Host";
  doc["files"]["f"]["fileName"] = name;
  doc["files"]["f"]["size"] = size;
  std::string out;
  serializeJson(doc, out);
  return out;
}
int prepare(Session& s, const std::string& json, uint32_t now = 100) {
  return s.prepare(json.data(), json.size(), 42, now, id, token);
}
int parse(std::string text, Request& r) { return parseRequest(text.data(), r); }
}  // namespace
int main() {
  std::memset(id, 'a', 32);
  std::memset(token, 'b', 32);
  for (const auto* name :
       {"Book.epub", "Book.EPUB", "A book.epub", "cover.PNG", "image.jpg", "a.JPEG", "b.bmp", "c.BMP"})
    require(safeReceiveName(name), "valid filename");
  for (const auto* name :
       {"../x.epub", "/x.epub", ".x.epub", "a/b.epub", "a\\b.epub", "a:book.epub", "a%2fb.epub", "CON.epub", "NUL.epub",
        "COM1.epub", "a.txt", "a.gif", "x.epub ", " a.epub", "a\nb.epub", "a?.epub", ".png", "a/cover.jpg"})
    require(!safeReceiveName(name), "unsafe filename");
  for (const auto* folder : {"/", "/Books", "/LocalSend", "/Books/LocalSend", "/.sleep", "/.sleep-overlay",
                             "/Books/Inbox", "/Books/Series/A"}) {
    require(safeDestinationFolder(folder), "safe destination folder");
  }
  for (const auto* folder :
       {"", "/Book", "/Bookshelf", "/Books/", "/Books//Bad", "/Books/../Bad", "/Books/.private", "/Books/Good/.",
        "/Books/a\\b", "/Books/a:b", "/Books/a%2Fb", "/Books/Bad\\nLine", "/Books/Good/"}) {
    require(!safeDestinationFolder(folder), "unsafe destination folder");
  }
  const uint8_t epub[] = {'P', 'K', 3, 4, 0, 0, 0, 0};
  const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe0, 0, 0, 0, 0};
  const uint8_t bmp[] = {'B', 'M', 0, 0, 0, 0, 0, 0};
  require(validFileSignature("book.epub", epub, 8) && !validFileSignature("book.epub", png, 8), "EPUB ZIP signature");
  require(validFileSignature("cover.png", png, 8) && !validFileSignature("cover.png", png, 7) &&
              !validFileSignature("cover.png", jpeg, 8),
          "PNG full signature");
  require(validFileSignature("photo.JPG", jpeg, 8) && validFileSignature("photo.jpeg", jpeg, 8) &&
              !validFileSignature("photo.jpg", bmp, 8),
          "JPEG signature");
  require(validFileSignature("wallpaper.bmp", bmp, 8) && !validFileSignature("wallpaper.bmp", png, 8), "BMP signature");
  require(!validFileSignature("script.txt", png, 8) && !validFileSignature("image.jpg", nullptr, 8),
          "unknown type or null bytes rejected");
  require(!safeDestinationFolder(nullptr), "null destination rejected");
  require(!safeDestinationFolder(("/Books/" + std::string(96, 'x')).c_str()), "oversized destination");
  uint32_t size;
  require(parseLength("1073741824", size) && size == MAX_FILE_BYTES, "maximum decimal size");
  for (const auto* text : {"", "-1", "+3", "3x", "1073741825", "999999999999999999999999"})
    require(!parseLength(text, size), "bad decimal length");
  const auto valid = offer();
  Session s;
  require(prepare(s, valid) == 200, "valid offer");
  const char* rejectionReason = nullptr;
  require(s.prepare(valid.data(), valid.size(), 42, 100, id, token, &rejectionReason) == 409 && rejectionReason &&
              !std::strcmp(rejectionReason, "another transfer is active"),
          "busy offer has actionable rejection reason");
  s.reset();
  const auto oversizedName = offer((std::string(130, 'a') + ".epub").c_str());
  rejectionReason = nullptr;
  require(s.prepare(oversizedName.data(), oversizedName.size(), 42, 100, id, token, &rejectionReason) == 400 &&
              rejectionReason && !std::strcmp(rejectionReason, "filename exceeds 128-byte limit") &&
              s.state == State::Idle,
          "oversized EPUB filename has specific rejection reason");
  const auto manga = offer("graphic-novel.epub", 700ull * 1024ull * 1024ull);
  require(prepare(s, manga) == 200 && s.files[0].expected == 700u * 1024u * 1024u,
          "700 MiB graphic novel accepted from metadata without loading payload");
  s.reset();
  const auto oversizedBook = offer("large.epub", MAX_FILE_BYTES + 1);
  rejectionReason = nullptr;
  require(s.prepare(oversizedBook.data(), oversizedBook.size(), 42, 100, id, token, &rejectionReason) == 400 &&
              rejectionReason && !std::strcmp(rejectionReason, "each file must be 4 bytes to 1 GiB") &&
              s.state == State::Idle,
          "oversized EPUB has specific size rejection reason");
  require(prepare(s, valid) == 200 && s.state == State::Prepared, "valid offer after rejected EPUBs");
  require(prepare(s, valid) == 409, "one active session");
  require(s.authorized(id, "f", token, 42, 101), "valid credentials");
  require(!s.authorized(id, "f", token, 43, 101), "peer binding");
  require(!s.authorized("bad", "f", token, 42, 101), "wrong session");
  require(!s.authorized(id, "wrong", token, 42, 101), "wrong file");
  require(!s.authorized(id, "f", "bad", 42, 101), "wrong token");
  require(!s.authorized(id, "f", token, 42, 120100), "expired credentials");
  s.state = State::Receiving;
  require(s.consume(s.files[0], 1024, 101) && !s.complete(s.files[0]), "partial stream");
  require(s.consume(s.files[0], 354, 102) && s.complete(s.files[0]), "exact completion");
  require(!s.consume(s.files[0], 1, 103) && !s.complete(s.files[0]), "overflow fails closed");
  s.reset();
  require(!s.authorized(id, "f", token, 42, 100), "no replay");
  for (size_t i = 0; i < valid.size(); ++i) {
    s.reset();
    require(prepare(s, valid.substr(0, i)) == 400, "all truncated offers");
  }
  for (const auto& json :
       {offer("../x.epub"), offer("x.epub", 0), offer("x.epub", 3), offer("x.epub", 1073741825ull),
        offer("x.epub", 4294967296ull), std::string("{}"), std::string("{\"info\":{},\"files\":[]}")}) {
    s.reset();
    require(prepare(s, json) == 400 && s.state == State::Idle, "bad offer leaves idle");
  }
  JsonDocument doc;
  deserializeJson(doc, valid);
  doc["files"]["f"]["sha256"] = std::string(64, 'A');
  std::string json;
  serializeJson(doc, json);
  s.reset();
  require(prepare(s, json) == 200 && s.files[0].sha256[0] == 'a', "hash normalized");
  doc["files"]["f"]["sha256"] = std::string(64, 'Z');
  json.clear();
  serializeJson(doc, json);
  s.reset();
  require(prepare(s, json) == 400, "bad hash");
  doc["files"]["f"]["sha256"] = nullptr;
  doc["files"]["g"]["fileName"] = "x.epub";
  doc["files"]["g"]["size"] = 1024;
  json.clear();
  serializeJson(doc, json);
  s.reset();
  require(prepare(s, json) == 200 && s.fileCount == 2 && s.files[1].expected == 1024,
          "two-file offer accepted with separate records");
  require(s.authorized(id, "g", token, 42, 100), "second-file credential bound to batch session");
  s.reset();
  doc["files"]["g"]["fileName"] = "Book.EPUB";
  json.clear();
  serializeJson(doc, json);
  require(prepare(s, json) == 400, "duplicate case-insensitive filename rejected in one offer");
  s.reset();
  doc["files"]["g"]["fileName"] = "folder/inside.epub";
  json.clear();
  serializeJson(doc, json);
  require(prepare(s, json) == 400, "folder paths rejected until safe nested paths are implemented");
  s.reset();
  doc["files"]["g"]["fileName"] = "x.epub";
  doc["files"]["f"]["size"] = MAX_FILE_BYTES;
  doc["files"]["g"]["size"] = MAX_FILE_BYTES;
  doc["files"]["h"]["fileName"] = "total.epub";
  doc["files"]["h"]["size"] = 4;
  json.clear();
  serializeJson(doc, json);
  require(prepare(s, json) == 200 && s.fileCount == 3,
          "large multi-GiB batch metadata allowed without payload allocation");
  s.reset();
  doc.clear();
  doc["info"]["alias"] = "Host";
  for (size_t i = 0; i < MAX_OFFER_FILES; ++i) {
    const std::string id = "file-" + std::to_string(i);
    doc["files"][id]["fileName"] = id + ".epub";
    doc["files"][id]["size"] = MAX_FILE_BYTES;
  }
  json.clear();
  serializeJson(doc, json);
  require(json.size() < MAX_METADATA, "16-file offer fits metadata budget");
  require(prepare(s, json) == 200 && s.fileCount == MAX_OFFER_FILES,
          "16-file offer accepted at exact batch limit with 16 GiB total metadata");
  s.reset();
  doc["files"]["extra"]["fileName"] = "extra.epub";
  doc["files"]["extra"]["size"] = 4;
  json.clear();
  serializeJson(doc, json);
  require(prepare(s, json) == 400, "17 files rejected before consent");
  s.reset();
  doc.clear();
  doc["info"]["alias"] = "Host";
  for (size_t i = 0; i < MAX_OFFER_FILES; ++i) {
    const std::string id = std::string(120, 'i') + std::to_string(i);
    doc["files"][id]["fileName"] = std::string(114, 'n') + std::to_string(i) + ".epub";
    doc["files"][id]["size"] = 4;
    doc["files"][id]["sha256"] = std::string(64, 'a');
  }
  json.clear();
  serializeJson(doc, json);
  require(json.size() < MAX_METADATA, "large 16-file metadata remains within request limit");
  require(prepare(s, json) == 200 && s.fileCount == MAX_OFFER_FILES,
          "16 long filenames, IDs and SHA-256 hashes accepted within bounded JSON");
  s.reset();
  doc.clear();
  doc["info"]["alias"] = "Host";
  doc["files"]["f"]["fileName"] = "f.epub";
  doc["files"]["f"]["size"] = MAX_FILE_BYTES;
  doc["files"]["g"]["fileName"] = "g.epub";
  doc["files"]["g"]["size"] = MAX_FILE_BYTES;
  json.clear();
  serializeJson(doc, json);
  require(prepare(s, json) == 200 && s.fileCount == 2,
          "2 GiB combined boundary accepted with streaming (no file allocation)");
  s.reset();
  require(prepare(s, valid, UINT32_MAX - 100) == 200 && !s.expired(50) && s.expired(120000), "timer rollover");
  BoundedJsonAllocator allocator;
  void* allocation = allocator.allocate(4096);
  require(allocation != nullptr, "bounded allocation");
  require(allocator.reallocate(allocation, 16384) == nullptr, "reallocation rejects budget overflow");
  allocator.deallocate(allocation);
  allocation = allocator.allocate(4096);
  require(allocation != nullptr, "budget reclaimed");
  allocator.deallocate(allocation);
  require(allocator.allocate(SIZE_MAX) == nullptr, "allocation size overflow rejected");
  JsonDocument large;
  deserializeJson(large, valid);
  auto padding = large["padding"].to<JsonArray>();
  for (int i = 0; i < 2000; ++i) padding.add(0);
  json.clear();
  serializeJson(large, json);
  s.reset();
  require(json.size() < MAX_METADATA && prepare(s, json) == 400, "JSON allocation exhaustion fails closed");
  uint32_t random = 7;
  for (int trial = 0; trial < 2000; ++trial) {
    auto mutated = valid;
    for (int j = 0; j < 4; ++j) {
      random = random * 1664525u + 1013904223u;
      mutated[random % mutated.size()] = char(random >> 24);
    }
    s.reset();
    const int result = prepare(s, mutated);
    require(result == 200 ? s.state == State::Prepared && safeReceiveName(s.files[0].fileName) &&
                                s.files[0].expected <= MAX_FILE_BYTES
                          : s.state == State::Idle,
            "mutated offer stays within contract");
  }
  Request r;
  require(parse("GET /api/localsend/v2/info HTTP/1.1\r\nHost: reader\r\n\r\n", r) == 200, "info request");
  require(parse("POST /api/localsend/v2/register HTTP/1.1\r\nContent-Length: 10\r\n\r\n", r) == 200,
          "register request");
  const std::string upload = std::string("POST /api/localsend/v2/upload?sessionId=") + id +
                             "&fileId=file%2D1&token=" + token +
                             " HTTP/1.1\r\nContent-Length: 1378\r\nExpect: 100-continue\r\n\r\n";
  require(parse(upload, r) == 200 && !std::strcmp(r.fileId, "file-1") && r.length == 1378 && r.expectContinue,
          "upload query and framing");
  for (const auto* text : {"POST /api/localsend/v2/upload HTTP/1.1\r\nContent-Length: 1\r\n\r\n",
                           "POST /api/localsend/v2/register HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n",
                           "POST /api/localsend/v2/register HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
                           "POST /api/localsend/v2/register HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                           "GET /api/localsend/v2/info HTTP/1.1\r\nContent-Length: 4\r\n\r\n"})
    require(parse(text, r) != 200, "bad framing");
  require(parse("POST /api/localsend/v2/prepare-upload HTTP/1.1\r\nContent-Length: 8193\r\n\r\n", r) == 413,
          "metadata cap");
  require(parse("POST /api/localsend/v2/register HTTP/1.1\r\n\r\n", r) == 411, "length required");
  require(parse("GET /api/localsend/v2/prepare-upload HTTP/1.1\r\n\r\n", r) == 405, "method rejected");
  require(parse("GET /unknown HTTP/1.1\r\n\r\n", r) == 404, "unknown route");
  for (const auto* bad : {"%00", "%", "%GG", "x&fileId=y"}) {
    auto text = upload;
    const auto pos = text.find("file%2D1");
    text.replace(pos, 8, bad);
    require(parse(text, r) == 400, "bad query");
  }
  auto chunkedUpload = upload;
  const auto lengthHeader = chunkedUpload.find("Content-Length: 1378");
  chunkedUpload.replace(lengthHeader, std::strlen("Content-Length: 1378"), "Transfer-Encoding: chunked");
  require(parse(chunkedUpload, r) == 200 && r.chunked && !r.length, "streamed upload without Content-Length");
  auto ambiguous = chunkedUpload;
  ambiguous.insert(ambiguous.find("Transfer-Encoding:"), "Content-Length: 1378\r\n");
  require(parse(ambiguous, r) == 400, "reject ambiguous body framing");
  auto duplicateEncoding = chunkedUpload;
  duplicateEncoding.insert(duplicateEncoding.find("Transfer-Encoding:"), "Transfer-Encoding: chunked\r\n");
  require(parse(duplicateEncoding, r) == 400, "reject duplicate transfer encoding");
  for (const auto* bad : {"gzip", "gzip, chunked", "chunked, chunked"}) {
    auto text = chunkedUpload;
    const auto at = text.find("Transfer-Encoding: chunked");
    text.replace(at, std::strlen("Transfer-Encoding: chunked"), std::string("Transfer-Encoding: ") + bad);
    require(parse(text, r) == 400, "reject unsupported encoding chains");
  }
  require(parse("POST /api/localsend/v2/cancel?sessionId=test HTTP/1.1\r\n\r\n", r) == 200,
          "empty sender cancellation without explicit length");
  for (const auto* validChunk : {"0", "0000", "1", "aBc", "2000000", "100;extension=value"}) {
    uint32_t size;
    require(parseChunkSize(validChunk, size) && size <= MAX_FILE_BYTES, "valid bounded chunk size");
  }
  for (const auto* badChunk : {"", "-1", "0x10", " 1", "1 ", "40000001", "FFFFFFFFFFFFFFFF", ";x", "1;bad\nvalue"}) {
    uint32_t size;
    require(!parseChunkSize(badChunk, size), "invalid or overflowing chunk size");
  }
  std::printf("PASS: %d LocalSend session/framing checks; session=%zu bytes; request=%zu bytes\n", checks,
              sizeof(Session), sizeof(Request));
}
