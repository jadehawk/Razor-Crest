#pragma once
constexpr int F_SETFL = 1, F_GETFL = 2, O_NONBLOCK = 4;
inline int fcntl(int, int, int = 0) { return -1; }
