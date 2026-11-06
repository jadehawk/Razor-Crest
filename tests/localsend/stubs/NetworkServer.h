#pragma once
// Arduino Print.h exposes this macro through NetworkServer.h.
#define HEX 16
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
inline uint32_t testMillis = 0;
inline uint32_t millis() { return testMillis; }
inline void delay(unsigned n) { testMillis += n; }
struct IPAddress {
  uint32_t address = 42;
  operator uint32_t() const { return address; }
  std::string toString() const { return "192.0.2.42"; }
};
struct TestConnection {
  std::string input, output;
  size_t position = 0;
  bool connected = true;
  uint32_t address = 42;
};
class NetworkClient {
 public:
  std::shared_ptr<TestConnection> connection;
  NetworkClient() = default;
  explicit NetworkClient(std::shared_ptr<TestConnection> c) : connection(std::move(c)) {}
  explicit operator bool() const { return bool(connection); }
  void setTimeout(unsigned) {}
  void setNoDelay(bool) {}
  int available() { return connection ? int(connection->input.size() - connection->position) : 0; }
  bool connected() { return connection && connection->connected; }
  int read(uint8_t* out, size_t length) {
    length = std::min(length, size_t(available()));
    std::memcpy(out, connection->input.data() + connection->position, length);
    connection->position += length;
    return int(length);
  }
  size_t write(const uint8_t* bytes, size_t length) {
    connection->output.append(reinterpret_cast<const char*>(bytes), length);
    return length;
  }
  void print(const char* text) { write(reinterpret_cast<const uint8_t*>(text), std::strlen(text)); }
  void stop() {
    if (connection) connection->connected = false;
  }
  IPAddress remoteIP() const { return IPAddress{connection->address}; }
};
inline std::shared_ptr<TestConnection> testPending;
class NetworkServer {
  bool active = false;

 public:
  NetworkServer(uint16_t, uint8_t) {}
  void begin() { active = true; }
  void end() { active = false; }
  explicit operator bool() const { return active; }
  NetworkClient accept() {
    NetworkClient client(testPending);
    testPending.reset();
    return client;
  }
};
