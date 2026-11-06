#include <ArduinoJson.h>
#include <HalStorage.h>
#include <NetworkServer.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/network/LocalSendReceiver.h"
namespace {
int checks = 0;
size_t cancelAt = SIZE_MAX;
std::shared_ptr<TestConnection> active;
void require(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
bool cancelled(void*) { return active && active->position >= cancelAt; }
std::string transact(LocalSendReceiver& receiver, const std::string& path, const std::string& body = "",
                     size_t declared = SIZE_MAX) {
  const bool get = path == "/api/localsend/v2/info";
  active = std::make_shared<TestConnection>();
  active->input = std::string(get ? "GET " : "POST ") + path + " HTTP/1.1\r\nHost: reader\r\nContent-Length: " +
                  std::to_string(declared == SIZE_MAX ? body.size() : declared) + "\r\n\r\n" + body;
  testPending = active;
  receiver.poll();
  return active->output;
}
std::string transactChunked(LocalSendReceiver& receiver, const std::string& path, const std::string& body,
                            const std::string& extraHeader = "") {
  active = std::make_shared<TestConnection>();
  active->input =
      "POST " + path + " HTTP/1.1\r\nHost: reader\r\nTransfer-Encoding: chunked\r\n" + extraHeader + "\r\n" + body;
  testPending = active;
  receiver.poll();
  return active->output;
}
std::string chunk(const std::string& bytes, const std::string& extension = "") {
  char size[32];
  std::snprintf(size, sizeof(size), "%zx", bytes.size());
  return std::string(size) + extension + "\r\n" + bytes + "\r\n";
}
bool status(const std::string& response, int code) {
  return response.find("HTTP/1.1 " + std::to_string(code) + " ") == 0;
}
JsonDocument jsonBody(const std::string& response) {
  JsonDocument doc;
  const auto at = response.find("\r\n\r\n");
  require(at != std::string::npos, "response header");
  require(!deserializeJson(doc, response.c_str() + at + 4), "response JSON");
  return doc;
}
std::string prepare(LocalSendReceiver& receiver, const std::string& name, size_t bytes, const char* hash = nullptr,
                    bool accept = true) {
  JsonDocument doc;
  doc["info"]["alias"] = "Host";
  doc["files"]["f"]["fileName"] = name;
  doc["files"]["f"]["size"] = bytes;
  if (hash) doc["files"]["f"]["sha256"] = hash;
  std::string body;
  serializeJson(doc, body);
  const auto response = transact(receiver, "/api/localsend/v2/prepare-upload", body);
  if (receiver.pendingOffer() && accept) {
    receiver.decide(true);
    return active->output;
  }
  return response;
}
std::string uploadUrl(const JsonDocument& doc, const std::string& fileId = "f") {
  return "/api/localsend/v2/upload?sessionId=" + doc["sessionId"].as<std::string>() + "&fileId=" + fileId +
         "&token=" + doc["files"][fileId].as<std::string>();
}
std::string prepareBatch(LocalSendReceiver& receiver,
                         const std::vector<std::pair<std::string, std::pair<std::string, size_t>>>& entries,
                         bool accept = true) {
  JsonDocument doc;
  doc["info"]["alias"] = "Host";
  for (const auto& [id, data] : entries) {
    doc["files"][id]["fileName"] = data.first;
    doc["files"][id]["size"] = data.second;
  }
  std::string body;
  serializeJson(doc, body);
  const auto response = transact(receiver, "/api/localsend/v2/prepare-upload", body);
  if (receiver.pendingOffer() && accept) {
    receiver.decide(true);
    return active->output;
  }
  return response;
}
bool noTemporary() {
  for (const auto& [path, bytes] : testFiles) {
    (void)bytes;
    if (path.ends_with(".part")) return false;
  }
  return true;
}
}  // namespace
int main() {
  LocalSendReceiver receiver;
  require(receiver.setAdvertisedId("X3") && receiver.begin(cancelled, nullptr), "start X3 receiver");

  auto info = jsonBody(transact(receiver, "/api/localsend/v2/info"));
  require(info["protocol"] == "http" && info["port"] == 53317 && info["alias"] == "Razor-Crest(X3)",
          "model-specific advertised identity");
  require(receiver.setAdvertisedId("User_Custom_ID"), "custom ID accepted");
  info = jsonBody(transact(receiver, "/api/localsend/v2/info"));
  require(info["alias"] == "Razor-Crest(User_Custom_ID)", "custom identity shown in HTTP info");
  require(!receiver.setAdvertisedId("") && !receiver.setAdvertisedId("../bad") &&
              !receiver.setAdvertisedId("name (bad)") && !receiver.setAdvertisedId(std::string(33, 'x').c_str()),
          "empty, unsafe or too-long custom IDs rejected");
  info = jsonBody(transact(receiver, "/api/localsend/v2/info"));
  require(info["alias"] == "Razor-Crest(User_Custom_ID)", "bad ID cannot overwrite active identity");
  require(receiver.setAdvertisedId("X4Pro"), "X4Pro identity accepted");
  info = jsonBody(transact(receiver, "/api/localsend/v2/info"));
  require(info["alias"] == "Razor-Crest(X4Pro)", "X4Pro alias visible to LocalSend clients");
  require(receiver.setAdvertisedId("X3"), "restore detected identity");
  require(status(transact(receiver, "/api/localsend/v2/register", "{\"fingerprint\":\"sender\"}"), 200),
          "registration");
  std::string book = "PK";
  book += char(3);
  book += char(4);
  book += std::string(2044, 'x');
  testCardTotalBytes = 1024 * 1024;
  testCardUsedBytes = testCardTotalBytes - 128;
  const auto noSpace = prepare(receiver, "no-space.epub", book.size());
  require(status(noSpace, 507) && noSpace.find("Not enough free space") != std::string::npos && noTemporary(),
          "SD capacity checked before accepting a large upload");
  testCardTotalBytes = testCardUsedBytes = 0;
  auto prepared = prepare(receiver, "first.epub", book.size());
  require(status(prepared, 200), "prepare first book");
  require(status(prepare(receiver, "busy.epub", book.size()), 409), "busy session");
  auto credentials = jsonBody(prepared);
  auto url = uploadUrl(credentials);
  const auto writes = testWriteCalls;
  auto badTokenUrl = url;
  badTokenUrl.back() = badTokenUrl.back() == 'a' ? 'b' : 'a';
  require(status(transact(receiver, badTokenUrl, book), 403) && testWriteCalls == writes, "bad token never writes");
  require(status(transact(receiver, url, book), 200), "first successful upload");
  require(std::string(testFiles["/Books/LocalSend/first.epub"].begin(),
                      testFiles["/Books/LocalSend/first.epub"].end()) == book,
          "exact saved bytes");
  require(noTemporary(), "successful stage removed");
  require(status(transact(receiver, url, book), 403), "completed session cannot replay");
  const auto duplicateBook = prepare(receiver, "first.epub", book.size());
  require(status(duplicateBook, 403) &&
              duplicateBook.find("File already exists in destination folder") != std::string::npos,
          "existing book preserved with actionable duplicate response");
  require(testFiles["/Books/LocalSend/first.epub"].size() == book.size(), "existing book size unchanged");
  require(receiver.completedSequence() == 1 && !std::strcmp(receiver.completedName(), "first.epub") &&
              receiver.completedBytes() == book.size(),
          "successful publication exposes completed filename and bytes");
  require(!receiver.setDestination("/Books/../escape") && !receiver.setDestination("/Other") &&
              !receiver.setDestination("/Books/Bad/"),
          "invalid destinations rejected");
  require(receiver.setDestination("/Books/Inbox") && std::string(receiver.destinationFolder()) == "/Books/Inbox",
          "plugin-selected inbox destination accepted");
  prepared = prepare(receiver, "inbox.epub", book.size());
  require(status(prepared, 200), "prepare upload into selected folder");
  credentials = jsonBody(prepared);
  require(status(transact(receiver, uploadUrl(credentials), book), 200), "publish into selected folder");
  require(testFiles.count("/Books/Inbox/inbox.epub") == 1 && receiver.completedSequence() == 2 &&
              !std::strcmp(receiver.completedName(), "inbox.epub"),
          "selected folder and success-only notification");
  // The same authenticated staging/rename path is used for every supported
  // image format and for root and sleep/overlay destinations.
  const std::string png = std::string("\211PNG\r\n\032\n", 8) + std::string(64, 'p');
  const std::string jpg = std::string("\377\330\377", 3) + std::string(64, 'j');
  const std::string bmp = "BM" + std::string(64, 'b');
  struct ReceiveCase {
    const char* directory;
    const char* name;
    const std::string* bytes;
    const char* saved;
  };
  const ReceiveCase cases[] = {{"/", "root.png", &png, "/root.png"},
                               {"/Books", "book-cover.jpg", &jpg, "/Books/book-cover.jpg"},
                               {"/LocalSend", "picture.bmp", &bmp, "/LocalSend/picture.bmp"},
                               {"/Books/LocalSend", "photo.jpeg", &jpg, "/Books/LocalSend/photo.jpeg"},
                               {"/.sleep", "sleep.JPG", &jpg, "/.sleep/sleep.JPG"},
                               {"/.sleep-overlay", "overlay.PNG", &png, "/.sleep-overlay/overlay.PNG"}};
  for (const auto& c : cases) {
    require(receiver.setDestination(c.directory), "supported destination accepted");
    prepared = prepare(receiver, c.name, c.bytes->size());
    require(status(prepared, 200), "prepare supported image file");
    credentials = jsonBody(prepared);
    require(status(transact(receiver, uploadUrl(credentials), *c.bytes), 200), "publish supported image file");
    const auto& bytes = testFiles[c.saved];
    require(std::string(bytes.begin(), bytes.end()) == *c.bytes, "image saved with exact bytes in requested folder");
    require(!std::strcmp(receiver.completedName(), c.name) && noTemporary(), "image success published only after save");
  }
  require(receiver.setDestination("/.sleep-overlay"), "overlay destination for chunked image");
  prepared = prepare(receiver, "chunked-overlay.png", png.size());
  require(status(prepared, 200), "chunked PNG prepare");
  credentials = jsonBody(prepared);
  const auto chunkedPng = chunk(png.substr(0, 1)) + chunk(png.substr(1, 4)) + chunk(png.substr(5)) + "0\r\n\r\n";
  require(status(transactChunked(receiver, uploadUrl(credentials), chunkedPng), 200), "chunked PNG upload");
  const auto& savedPng = testFiles["/.sleep-overlay/chunked-overlay.png"];
  require(std::string(savedPng.begin(), savedPng.end()) == png && noTemporary(),
          "chunked image published with exact bytes and no staging residue");
  require(receiver.setDestination("/"), "root destination reselect");
  const auto duplicatePng = prepare(receiver, "root.png", png.size());
  require(
      status(duplicatePng, 403) && duplicatePng.find("File already exists in destination folder") != std::string::npos,
      "duplicate at SD root rejected with clear reason");
  require(std::string(testFiles["/root.png"].begin(), testFiles["/root.png"].end()) == png,
          "duplicate preserves original PNG");
  prepared = prepare(receiver, "bad.png", book.size());
  require(status(prepared, 200), "offer with misleading image extension can prepare");
  credentials = jsonBody(prepared);
  const auto beforeBadImage = receiver.completedSequence();
  require(status(transact(receiver, uploadUrl(credentials), book), 422), "invalid image signature rejected");
  require(receiver.completedSequence() == beforeBadImage && testFiles.count("/bad.png") == 0 && noTemporary(),
          "invalid image not published or recorded");
  prepared = prepare(receiver, "not-jpeg.jpg", png.size());
  require(status(prepared, 200), "mislabeled JPEG offer may prepare");
  credentials = jsonBody(prepared);
  url = uploadUrl(credentials);
  const std::string mismatchedJpg = chunk(png.substr(0, 2)) + chunk(png.substr(2)) + "0\r\n\r\n";
  require(status(transactChunked(receiver, url, mismatchedJpg), 422),
          "mislabeled chunked JPEG rejected at signature validation");
  require(status(transact(receiver, url, png), 403), "failed JPEG session token cannot replay");
  require(testFiles.count("/not-jpeg.jpg") == 0 && noTemporary() && receiver.completedSequence() == beforeBadImage,
          "rejected JPEG does not publish or appear in completed history");
  prepared = prepare(receiver, "chunked-valid.JPG", jpg.size());
  require(status(prepared, 200), "valid JPEG offer may prepare");
  credentials = jsonBody(prepared);
  const std::string chunkedJpg = chunk(jpg.substr(0, 1)) + chunk(jpg.substr(1, 1)) + chunk(jpg.substr(2)) + "0\r\n\r\n";
  require(status(transactChunked(receiver, uploadUrl(credentials), chunkedJpg), 200),
          "valid chunked JPEG accepted with marker split across chunks");
  require(testFiles.count("/chunked-valid.JPG") == 1 && noTemporary(), "valid JPEG published exactly once");
  prepared = prepare(receiver, "interrupted.jpg", jpg.size());
  require(status(prepared, 200), "interrupted JPEG offer accepted");
  credentials = jsonBody(prepared);
  url = uploadUrl(credentials);
  require(status(transactChunked(receiver, url, chunk(jpg.substr(0, 16))), 408),
          "interrupted chunked JPEG upload returns timeout");
  require(noTemporary() && testFiles.count("/interrupted.jpg") == 0,
          "interrupted JPEG preserves storage and removes private staging");
  require(status(transactChunked(receiver, url, chunk(jpg) + "0\r\n\r\n"), 403),
          "aborted upload token cannot be replayed");
  prepared = prepare(receiver, "new-attempt.jpg", jpg.size());
  require(status(prepared, 200), "fresh JPEG offer accepted after interrupted transfer");
  credentials = jsonBody(prepared);
  require(status(transactChunked(receiver, uploadUrl(credentials), chunk(jpg) + "0\r\n\r\n"), 200),
          "fresh JPEG upload after interruption succeeds");
  require(testFiles.count("/new-attempt.jpg") == 1 && noTemporary(), "fresh JPEG publishes with no staging residue");
  const auto longEpubName = std::string(130, 'x') + ".epub";
  auto rejected = prepare(receiver, longEpubName, book.size());
  require(status(rejected, 400) && rejected.find("filename exceeds 128-byte limit") != std::string::npos,
          "oversized EPUB filename rejected with reason after successful JPG");
  rejected = prepare(receiver, "oversized.epub", localsend::MAX_FILE_BYTES + 1);
  require(status(rejected, 400) && rejected.find("each file must be 4 bytes to 1 GiB") != std::string::npos,
          "oversized EPUB offer rejected with reason after successful JPG");
  prepared = prepare(receiver, "after-jpg.epub", book.size());
  require(status(prepared, 200), "valid EPUB offer accepted after JPG and rejected EPUB metadata");
  credentials = jsonBody(prepared);
  require(status(transactChunked(receiver, uploadUrl(credentials), chunk(book) + "0\r\n\r\n"), 200),
          "valid EPUB saves after previously accepted JPG");
  require(testFiles.count("/after-jpg.epub") == 1 && noTemporary(),
          "sequential JPG then EPUB publishes the second file");
  const auto sequenceBeforeBatch = receiver.completedSequence();
  prepared = prepareBatch(receiver, {{"image", {"batch-image.png", png.size()}},
                                     {"book", {"batch-book.epub", book.size()}},
                                     {"photo", {"batch-photo.jpg", jpg.size()}}});
  require(status(prepared, 200), "three-file offer uses one acceptance");
  credentials = jsonBody(prepared);
  require(credentials["files"].as<JsonObjectConst>().size() == 3, "all accepted files receive a token");
  require(credentials["files"]["image"] != credentials["files"]["book"] &&
              credentials["files"]["book"] != credentials["files"]["photo"],
          "file tokens are unique within the same batch");
  require(status(transactChunked(receiver, uploadUrl(credentials, "photo"), chunk(jpg) + "0\r\n\r\n"), 200),
          "third offered JPG can upload first");
  require(status(transact(receiver, uploadUrl(credentials, "photo"), jpg), 403),
          "completed file token cannot replay while batch remains active");
  require(status(transact(receiver, uploadUrl(credentials, "book"), book), 200),
          "second offered EPUB uploads with retained session");
  require(status(transactChunked(receiver, uploadUrl(credentials, "image"), chunk(png) + "0\r\n\r\n"), 200),
          "last offered PNG completes the session");
  require(receiver.completedSequence() == sequenceBeforeBatch + 3 && noTemporary() &&
              testFiles.count("/batch-image.png") == 1 && testFiles.count("/batch-book.epub") == 1 &&
              testFiles.count("/batch-photo.jpg") == 1,
          "three file types published exactly once without temp residue");
  require(status(transact(receiver, uploadUrl(credentials, "book"), book), 403),
          "batch token rejected after final completion");
  const auto beforeLargeBatch = receiver.completedSequence();
  std::vector<std::pair<std::string, std::pair<std::string, size_t>>> sixteen;
  for (int i = 0; i < 16; ++i) {
    const std::string id = "id" + std::to_string(i);
    sixteen.push_back({id, {"multi-" + std::to_string(i) + ".epub", book.size()}});
  }
  prepared = prepareBatch(receiver, sixteen);
  require(status(prepared, 200), "16-file batch accepted by HTTP receiver");
  credentials = jsonBody(prepared);
  require(credentials["files"].as<JsonObjectConst>().size() == 16, "16 distinct upload tokens returned");
  for (const auto& pair : sixteen) {
    const std::string id = pair.first;
    require(status(transact(receiver, uploadUrl(credentials, id), book), 200), "every file in 16-file batch received");
    require(testFiles.count("/" + pair.second.first) == 1, "16-file batch writes each file");
  }
  require(receiver.completedSequence() == beforeLargeBatch + 16 && noTemporary(),
          "16-file batch completes with no staging leftovers");
  const auto beforeBadBatch = receiver.completedSequence();
  prepared =
      prepareBatch(receiver, {{"good", {"multi-good.epub", book.size()}}, {"bad", {"multi-bad.jpg", png.size()}}});
  require(status(prepared, 200), "batch with later invalid file is accepted pending verification");
  credentials = jsonBody(prepared);
  require(status(transact(receiver, uploadUrl(credentials, "good"), book), 200),
          "first file of later-bad batch is published");
  require(status(transact(receiver, uploadUrl(credentials, "bad"), png), 422),
          "invalid second file is rejected at signature validation");
  require(status(transact(receiver, uploadUrl(credentials, "bad"), png), 403), "failed batch token is consumed");
  require(receiver.completedSequence() == beforeBadBatch + 1 && testFiles.count("/multi-good.epub") == 1 &&
              testFiles.count("/multi-bad.jpg") == 0 && noTemporary(),
          "earlier valid file survives later failed batch upload");
  prepared = prepareBatch(receiver, {{"new", {"subsequent.epub", book.size()}}});
  require(status(prepared, 200), "fresh batch after failed batch is allowed");
  credentials = jsonBody(prepared);
  require(status(transact(receiver, uploadUrl(credentials, "new"), book), 200), "next batch succeeds after failure");
  const auto duplicateBefore = receiver.completedSequence();
  const auto duplicateBatch =
      prepareBatch(receiver, {{"a", {"batch-photo.jpg", jpg.size()}}, {"b", {"never-created.epub", book.size()}}});
  require(status(duplicateBatch, 403) && receiver.completedSequence() == duplicateBefore &&
              testFiles.count("/never-created.epub") == 0,
          "batch rejected atomically when one destination already exists");
  require(receiver.setDestination("/Books/LocalSend"), "restore default for remaining tests");
  for (int fault = 0; fault < 7; ++fault) {
    const std::string name = "case-" + std::to_string(fault) + ".epub";
    const std::string hash(64, fault == 4 ? '1' : '0');
    prepared = prepare(receiver, name, book.size(), fault == 0 || fault == 4 ? hash.c_str() : nullptr);
    require(status(prepared, 200), "prepare fault scenario");
    credentials = jsonBody(prepared);
    url = uploadUrl(credentials);
    testShortWrite = fault == 1;
    testCloseFails = fault == 2;
    testRenameFails = fault == 3;
    if (fault == 6) cancelAt = 1024;
    const auto response = transact(receiver, url, fault == 5 ? book.substr(0, 100) : book, book.size());
    cancelAt = SIZE_MAX;
    testShortWrite = testCloseFails = testRenameFails = false;
    if (!status(response, fault == 0 ? 200 : fault == 4 ? 422 : fault >= 5 ? 408 : 500))
      std::fprintf(stderr, "fault=%d response=%s\n", fault, response.c_str());
    require(status(response, fault == 0 ? 200 : fault == 4 ? 422 : fault >= 5 ? 408 : 500), "fault response code");
    require(noTemporary(), "failed staging file removed");
    require(testFiles.count("/Books/LocalSend/" + name) == size_t(fault == 0), "publish only complete successful file");
  }
  prepared = prepare(receiver, "expired.epub", book.size());
  credentials = jsonBody(prepared);
  url = uploadUrl(credentials);
  testMillis += localsend::SESSION_TIMEOUT_MS;
  require(status(transact(receiver, url, book), 403), "expired session rejected");
  prepared = prepare(receiver, "cancelled.epub", book.size());
  credentials = jsonBody(prepared);
  require(status(transact(receiver, "/api/localsend/v2/cancel?sessionId=" + credentials["sessionId"].as<std::string>()),
                 200),
          "sender cancellation");
  require(status(transact(receiver, uploadUrl(credentials), book), 403), "cancelled token rejected");
  for (int fault = 0; fault < 10; ++fault) {
    receiver.stop();
    require(receiver.begin(cancelled, nullptr), "restart for chunked transaction");
    const auto name = "chunked-" + std::to_string(fault) + ".epub";
    const auto preparedChunked = prepare(receiver, name, book.size());
    require(status(preparedChunked, 200), "prepare chunked transfer");
    auto auth = jsonBody(preparedChunked);
    auto path = uploadUrl(auth);
    std::string encoded;
    // Split ZIP magic across chunks; extension and trailer are legal framing.
    encoded = chunk(book.substr(0, 1)) + chunk(book.substr(1, 2), ";test=1") + chunk(book.substr(3)) +
              "0\r\nX-Note: ok\r\n\r\n";
    int expected = 200;
    switch (fault) {
      case 0:
        break;
      case 1:
        encoded = chunk(book + "x") + "0\r\n\r\n";
        expected = 400;
        break;
      case 2:
        encoded = chunk(book.substr(0, 1)) + "0\r\n\r\n";
        expected = 422;
        break;
      case 3:
        encoded = chunk(book);
        expected = 408;
        break;
      case 4:
        encoded = chunk(book);
        encoded.back() = 'x';
        expected = 400;
        break;
      case 5:
        encoded = "nothex\r\n";
        expected = 400;
        break;
      case 6:
        encoded = chunk(book) + "0\r\ninvalid-trailer\r\n\r\n";
        expected = 400;
        break;
      case 7:
        encoded = "800\r\n" + book.substr(0, 100);
        expected = 408;
        break;
      case 8:
        path.back() = path.back() == 'a' ? 'b' : 'a';
        expected = 403;
        break;
      case 9:
        encoded = "FFFFFFFFFFFFFFFF\r\n";
        expected = 400;
        break;
      default:
        break;
    }
    const auto writesBefore = testWriteCalls;
    const auto response = transactChunked(receiver, path, encoded);
    require(status(response, expected), "chunked success or framing failure");
    require(noTemporary(), "chunked failure removes staging");
    require(testFiles.count("/Books/LocalSend/" + name) == size_t(fault == 0),
            "chunked publication only after complete framing");
    if (fault == 8) require(testWriteCalls == writesBefore, "chunked unauthorized body never writes");
    if (fault == 0) {
      const auto& saved = testFiles["/Books/LocalSend/" + name];
      require(std::string(saved.begin(), saved.end()) == book, "exact chunk-decoded EPUB");
    }
  }
  for (int choice = 0; choice < 7; ++choice) {
    receiver.stop();
    require(receiver.begin(cancelled, nullptr), "start consent scenario");
    const auto writesBefore = testWriteCalls;
    const auto waiting = prepare(receiver, "approval-" + std::to_string(choice) + ".epub", book.size(), nullptr, false);
    require(waiting.empty() && active->connected, "prepare response deferred while waiting");
    require(receiver.pendingOffer() != nullptr && receiver.pendingOffer()->state == localsend::State::AwaitingConsent,
            "explicit pending consent state");
    require(testWriteCalls == writesBefore && noTemporary(), "no SD write before consent");
    const auto pending = active;
    switch (choice) {
      case 0:
        receiver.decide(false);
        require(status(pending->output, 403) && pending->output.find("Transfer declined") != std::string::npos,
                "decline returns specific rejection");
        break;
      case 1:
        testMillis += localsend::CONSENT_TIMEOUT_MS;
        receiver.pollConsent();
        require(
            status(pending->output, 403) && pending->output.find("Transfer acceptance timed out") != std::string::npos,
            "timeout rejects pending offer with specific reason");
        break;
      case 2:
        pending->connected = false;
        receiver.pollConsent();
        break;
      case 3:
        testMillis += localsend::CONSENT_TIMEOUT_MS;
        receiver.decide(true);
        require(status(pending->output, 403), "late acceptance cannot revive offer");
        break;
      case 4:
        receiver.decide(true);
        require(status(pending->output, 200), "explicit acceptance returns credentials");
        break;
      case 5:
        receiver.stop();
        break;
      case 6: {
        // Simulate a file arriving after Prepare but before the user presses Accept.
        const std::string path = "/Books/LocalSend/approval-6.epub";
        testFiles[path] = {'P', 'K', 3, 4};
        receiver.decide(true);
        require(status(pending->output, 403) &&
                    pending->output.find("File already exists in destination folder") != std::string::npos,
                "late duplicate gets a specific rejection, not an overwrite");
        require(testFiles[path] == std::vector<uint8_t>({'P', 'K', 3, 4}),
                "late duplicate leaves the existing file untouched");
        break;
      }
      default:
        break;
    }
    require(!receiver.pendingOffer() && !pending->connected, "pending connection released");
    require(testWriteCalls == writesBefore && noTemporary(), "decision does not write book data");
    receiver.decide(true);
    require(!receiver.pendingOffer(), "no replay of stale UI decision");
  }
  receiver.stop();
  require(receiver.completedSequence() == 0 && !*receiver.completedName(), "stop clears session completion");
  require(noTemporary(), "receiver stop clean");
  require(testNestedStagingDirEnsured && !testBadStagingPath,
          "all transfers stage inside dedicated /.crosspoint/localsend directory");
  std::printf("PASS: %d LocalSend receiver checks; real protocol code with fake network/storage and digest sentinel\n",
              checks);
}
