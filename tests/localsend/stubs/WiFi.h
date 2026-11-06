#pragma once
#include "NetworkServer.h"
constexpr int WL_CONNECTED = 3;
struct TestWifi {
  int status() const { return WL_CONNECTED; }
  IPAddress localIP() const { return {}; }
};
inline TestWifi WiFi;
