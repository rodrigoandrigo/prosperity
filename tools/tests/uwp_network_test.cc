#include "host_network.h"
#include <poll.h>
#include <cstdio>
#include <cstring>
#define REQUIRE(value) do { if (!(value)) { std::printf("FAIL line %d errno=%d WSA=%d\n", __LINE__, errno, WSAGetLastError()); return 1; } } while (0)
int main() {
  const auto first = host_network::Socket(AF_INET, SOCK_DGRAM, 0);
  const auto second = host_network::Socket(AF_INET, SOCK_DGRAM, 0);
  REQUIRE(first >= 0 && second >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(host_network::Bind(second, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  int size = sizeof(address);
  REQUIRE(host_network::Getsockname(second, reinterpret_cast<sockaddr*>(&address), &size) == 0);
  pollfd readable{second, 1, 0};
  REQUIRE(poll(&readable, 1, 0) == 0);
  const char message[] = "Prosperity UWP UDP";
  REQUIRE(host_network::Sendto(first, message, sizeof(message), 0, reinterpret_cast<sockaddr*>(&address), size) == sizeof(message));
  REQUIRE(poll(&readable, 1, 1000) == 1 && (readable.revents & 1));
  fd_set read; FD_ZERO(&read); FD_SET(second, &read);
  timeval timeout{};
  REQUIRE(host_network::Select(second + 1, &read, nullptr, nullptr, &timeout) == 1 && FD_ISSET(second, &read));
  char buffer[64];
  REQUIRE(host_network::Recvfrom(second, buffer, sizeof(buffer), MSG_DONTWAIT, nullptr, nullptr) == sizeof(message));
  REQUIRE(std::memcmp(buffer, message, sizeof(message)) == 0);
  REQUIRE(host_network::Recvfrom(second, buffer, sizeof(buffer), MSG_DONTWAIT, nullptr, nullptr) == -1 && errno == EAGAIN);
  REQUIRE(host_network::Close(first) == 0 && host_network::Close(second) == 0);
  std::puts("Windows UDP: loopback send/receive, POSIX poll bits, select and nonblocking errno passed");
}
