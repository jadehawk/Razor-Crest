#pragma once
#include <cstddef>
#include <cstdint>
#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define IPPROTO_IP 0
#define IP_ADD_MEMBERSHIP 1
#define IP_MULTICAST_IF 2
#define INADDR_ANY 0
struct in_addr {
  uint32_t s_addr = 0;
};
struct sockaddr {};
struct sockaddr_in {
  int sin_family = 0;
  uint16_t sin_port = 0;
  in_addr sin_addr;
};
struct ip_mreq {
  in_addr imr_multiaddr, imr_interface;
};
inline uint16_t htons(uint16_t n) { return n; }
inline uint32_t htonl(uint32_t n) { return n; }
inline uint32_t inet_addr(const char*) { return 0; }
// Multicast is deliberately unavailable in host receiver tests.
inline int socket(int, int, int) { return -1; }
inline int bind(int, const sockaddr*, size_t) { return -1; }
inline int setsockopt(int, int, int, const void*, size_t) { return -1; }
inline int sendto(int, const void*, size_t, int, const sockaddr*, size_t) { return -1; }
inline int recv(int, void*, size_t, int) { return -1; }
