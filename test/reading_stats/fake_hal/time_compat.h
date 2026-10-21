#pragma once
#include <ctime>
#ifdef _WIN32
inline tm* localtime_r(const time_t* when, tm* output) {
  return localtime_s(output, when) == 0 ? output : nullptr;
}
#endif
