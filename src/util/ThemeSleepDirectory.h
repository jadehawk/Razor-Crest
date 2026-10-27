#pragma once
#include <cstring>
#include <string>

// Fixed, package-local sleep gallery directory. Never accepts traversal,
// nested folders, or files belonging to another installed theme.
namespace themesleep {
inline std::string folder(const std::string& id) { return "themes/" + id + "/sleep"; }
inline bool directory(const char* candidate, const std::string& id) { return candidate && folder(id) == candidate; }
inline bool image(const std::string& candidate, const std::string& id) {
  const std::string prefix = folder(id) + "/";
  if (candidate.compare(0, prefix.size(), prefix) != 0) return false;
  const std::string filename = candidate.substr(prefix.size());
  if (filename.size() < 5 || filename.size() > 84 || filename[0] == '.' ||
      (filename.compare(filename.size() - 4, 4, ".cpb") != 0 &&
       filename.compare(filename.size() - 4, 4, ".bmp") != 0) ||
      filename.find("..") != std::string::npos)
    return false;
  for (char ch : filename)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
          ch == '.'))
      return false;
  return true;
}
}  // namespace themesleep
