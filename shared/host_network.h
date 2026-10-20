#pragma once
#include <cstdint>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
namespace host_network {
#if defined(DELTA_UWP)
inline constexpr auto Socket = &uwp::Socket;
inline constexpr auto Bind = &uwp::Bind;
inline constexpr auto Getsockname = &uwp::Getsockname;
inline constexpr auto Sendto = &uwp::Sendto;
inline constexpr auto Send = &uwp::Send;
inline constexpr auto Recvfrom = &uwp::Recvfrom;
inline int Close(intptr_t socket) { return closesocket(socket) == SOCKET_ERROR ? uwp::SocketError() : 0; }
inline bool Nonblocking(intptr_t) { return false; } // Guest mode; host sockets always use bounded polling.
inline int Select(intptr_t, fd_set* read, fd_set* write, fd_set* except, timeval* timeout) {
  const int result = ::select(0, read, write, except, timeout);
  return result == SOCKET_ERROR ? uwp::SocketError() : result;
}
#else
inline constexpr auto Socket = &::socket;
inline constexpr auto Bind = &::bind;
inline constexpr auto Getsockname = &::getsockname;
inline constexpr auto Sendto = &::sendto;
inline constexpr auto Send = &::send;
inline constexpr auto Recvfrom = &::recvfrom;
inline int Close(intptr_t socket) { return ::close(static_cast<int>(socket)); }
inline bool Nonblocking(intptr_t socket) { return (fcntl(static_cast<int>(socket), F_GETFL) & O_NONBLOCK) != 0; }
inline int Select(intptr_t count, fd_set* read, fd_set* write, fd_set* except, timeval* timeout) {
  return ::select(static_cast<int>(count), read, write, except, timeout);
}
#endif
}
