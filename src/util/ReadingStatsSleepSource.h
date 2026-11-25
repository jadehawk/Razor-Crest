#pragma once

#include <string>
#include <vector>

namespace readingstatssleep {
inline constexpr const char* kId = "reading-statistics";
inline constexpr char kEnabledFile[] = "/stats-on-sleep";
inline constexpr char kCaptureRequest[] = "/sleep-frame.capture";
inline constexpr char kServerImage[] = "/sleep-server.bmp";
inline constexpr char kDeviceImage[] = "/sleep-device.bmp";
std::vector<std::string> imageCandidates();
bool enabled(const std::string& dir);
bool enabled();
}  // namespace readingstatssleep
