#include "CpbBitmap.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace themebitmap {
namespace {
constexpr uint32_t MAX_PIXELS = 1024u * 1024u;
constexpr uint32_t MAX_BYTES = 128u * 1024u;
constexpr uint32_t HEADER_SIZE = 20;

uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }
uint32_t read32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t updateCrc(uint32_t crc, const uint8_t byte) {
  crc ^= byte;
  for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  return crc;
}

struct Header {
  uint16_t w;
  uint16_t h;
  uint16_t stride;
  uint32_t bytes;
};

bool check(HalFile& file, Header& info) {
  if (!file || file.isDirectory()) return false;
  uint8_t header[HEADER_SIZE];
  if (file.fileSize64() < HEADER_SIZE || !file.seek(0) ||
      file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)))
    return false;
  if (std::memcmp(header, "CPTB", 4) || header[4] != 1 || header[5] != 1) return false;
  info = {read16(header + 6), read16(header + 8), read16(header + 10), read32(header + 12)};
  if (!info.w || !info.h || info.w > 1024 || info.h > 1024 || static_cast<uint32_t>(info.w) * info.h > MAX_PIXELS ||
      info.stride != (info.w + 7u) / 8u || info.bytes != static_cast<uint32_t>(info.stride) * info.h ||
      info.bytes > MAX_BYTES || file.fileSize64() != HEADER_SIZE + info.bytes)
    return false;
  const uint8_t paddingMask = info.w % 8 ? static_cast<uint8_t>((1u << (8u - (info.w % 8))) - 1u) : 0;
  const uint32_t expected = read32(header + 16);
  uint32_t crc = 0xFFFFFFFFu;
  uint8_t row[128];
  for (uint16_t y = 0; y < info.h; ++y) {
    if (file.read(row, info.stride) != info.stride) return false;
    if (paddingMask && (row[info.stride - 1] & paddingMask)) return false;
    for (uint16_t i = 0; i < info.stride; ++i) crc = updateCrc(crc, row[i]);
  }
  return (crc ^ 0xFFFFFFFFu) == expected;
}

struct Placement {
  int destX;
  int destY;
  int width;
  int height;
};

Placement fit(const Header& image, const Options& box) {
  int width = image.w;
  int height = image.h;
  if (box.fit != Fit::Native) {
    const bool useWidth = (static_cast<int64_t>(box.width) * image.h < static_cast<int64_t>(box.height) * image.w) ==
                          (box.fit == Fit::Contain);
    if (useWidth) {
      width = box.width;
      height = static_cast<int>(static_cast<int64_t>(image.h) * width / image.w);
    } else {
      height = box.height;
      width = static_cast<int>(static_cast<int64_t>(image.w) * height / image.h);
    }
    width = std::max(1, width);
    height = std::max(1, height);
  }
  return {box.x + (box.width - width) / 2, box.y + (box.height - height) / 2, width, height};
}
}  // namespace

bool probe(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("THEME", path, file) || file.isDirectory()) return false;
  uint8_t header[HEADER_SIZE] = {};
  if (file.fileSize64() < HEADER_SIZE || !file.seek(0) ||
      file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) || std::memcmp(header, "CPTB", 4) != 0 ||
      header[4] != 1 || header[5] != 1)
    return false;
  const uint16_t width = read16(header + 6);
  const uint16_t height = read16(header + 8);
  const uint16_t stride = read16(header + 10);
  const uint32_t bytes = read32(header + 12);
  return width > 0 && height > 0 && width <= 1024 && height <= 1024 &&
         static_cast<uint32_t>(width) * height <= MAX_PIXELS && stride == (width + 7u) / 8u &&
         bytes == static_cast<uint32_t>(stride) * height && bytes <= MAX_BYTES &&
         file.fileSize64() == HEADER_SIZE + bytes;
}

bool validate(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("THEME", path, file)) return false;
  Header header{};
  return check(file, header);
}

bool draw(const GfxRenderer& renderer, const std::string& path, const Options& options) {
  if (options.width <= 0 || options.height <= 0) return false;
  HalFile file;
  if (!Storage.openFileForRead("THEME", path, file)) return false;
  Header image{};
  if (!check(file, image)) {
    LOG_ERR("THEME", "Corrupt CPB: %s", path.c_str());
    return false;
  }

  const Placement p = fit(image, options);
  const int minX = std::max({options.x, p.destX, 0});
  const int maxX = std::min({options.x + options.width, p.destX + p.width, renderer.getScreenWidth()});
  const int minY = std::max({options.y, p.destY, 0});
  const int maxY = std::min({options.y + options.height, p.destY + p.height, renderer.getScreenHeight()});
  if (minX >= maxX || minY >= maxY) return true;
  uint8_t row[128];
  int lastSourceY = -1;
  for (int dy = minY; dy < maxY; ++dy) {
    const int sourceY = static_cast<int>(static_cast<int64_t>(dy - p.destY) * image.h / p.height);
    if (sourceY < 0 || sourceY >= image.h) return false;
    if (lastSourceY != sourceY) {
      const size_t offset = HEADER_SIZE + static_cast<size_t>(sourceY) * image.stride;
      if (!file.seek(offset) || file.read(row, image.stride) != image.stride) return false;
      lastSourceY = sourceY;
    }
    for (int dx = minX; dx < maxX; ++dx) {
      const int sourceX = static_cast<int>(static_cast<int64_t>(dx - p.destX) * image.w / p.width);
      if (sourceX < 0 || sourceX >= image.w) return false;
      const bool ink = (row[sourceX / 8] & (0x80u >> (sourceX % 8))) != 0;
      if (ink || !options.inkOnly) renderer.drawPixel(dx, dy, ink ? options.inkBlack : !options.inkBlack);
    }
  }
  return true;
}
}  // namespace themebitmap
