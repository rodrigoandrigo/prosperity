#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <mutex>
#include <vector>
using socklen_t = int;
namespace uwp {
inline int SocketError() {
  switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: errno = EAGAIN; break;
    case WSAEINTR: errno = EINTR; break;
    case WSAEACCES: errno = EACCES; break;
    case WSAEINVAL: errno = EINVAL; break;
    case WSAENOTSOCK: errno = EBADF; break;
    case WSAEADDRINUSE: errno = EADDRINUSE; break;
    case WSAEADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
    case WSAENETUNREACH: errno = ENETUNREACH; break;
    case WSAECONNRESET: errno = ECONNRESET; break;
    case WSAECONNREFUSED: errno = ECONNREFUSED; break;
    case WSAETIMEDOUT: errno = ETIMEDOUT; break;
    case WSAEMSGSIZE: errno = EMSGSIZE; break;
    default: errno = EIO; break;
  }
  return -1;
}
inline intptr_t Socket(int family, int type, int protocol) {
  static std::once_flag once;
  static int startup;
  std::call_once(once, [] { WSADATA data{}; startup = WSAStartup(MAKEWORD(2,2), &data); });
  if (startup) { WSASetLastError(startup); return SocketError(); }
  SOCKET socket = ::socket(family, type, protocol);
  if (socket == INVALID_SOCKET) return SocketError();
  u_long nonblocking = 1;
  if (ioctlsocket(socket, FIONBIO, &nonblocking)) { closesocket(socket); return SocketError(); }
  return static_cast<intptr_t>(socket);
}
inline int Bind(intptr_t fd, const sockaddr* address, int size) {
  return ::bind(fd, address, size) == SOCKET_ERROR ? SocketError() : 0;
}
inline int Getsockname(intptr_t fd, sockaddr* address, int* size) {
  return ::getsockname(fd, address, size) == SOCKET_ERROR ? SocketError() : 0;
}
inline int Sendto(intptr_t fd, const void* buffer, size_t size, int flags, const sockaddr* address, int address_size) {
  if (size > INT_MAX) { errno = EMSGSIZE; return -1; }
  const int result = ::sendto(fd, static_cast<const char*>(buffer), static_cast<int>(size), flags, address, address_size);
  return result == SOCKET_ERROR ? SocketError() : result;
}
inline int Send(intptr_t fd, const void* buffer, size_t size, int flags) {
  if (size > INT_MAX) { errno = EMSGSIZE; return -1; }
  const int result = ::send(fd, static_cast<const char*>(buffer), static_cast<int>(size), flags);
  return result == SOCKET_ERROR ? SocketError() : result;
}
constexpr int kDontWait = 0x100000;
inline int Recvfrom(intptr_t fd, void* buffer, size_t size, int flags, sockaddr* address, int* address_size) {
  if (size > INT_MAX) { errno = EMSGSIZE; return -1; }
  const int result = ::recvfrom(fd, static_cast<char*>(buffer), static_cast<int>(size), flags & ~kDontWait, address, address_size);
  return result == SOCKET_ERROR ? SocketError() : result;
}
// POSIX/FreeBSD event bits differ from WSAPoll's values.
struct PollFd { intptr_t fd; short events; short revents; };
inline int Poll(PollFd* descriptors, size_t count, int timeout) {
  if (count > ULONG_MAX) { errno = EINVAL; return -1; }
  std::vector<WSAPOLLFD> native(count);
  for (size_t i = 0; i < count; ++i) {
    short events = 0;
    if (descriptors[i].events & 1) events |= POLLRDNORM;
    if (descriptors[i].events & 2) events |= POLLPRI;
    if (descriptors[i].events & 4) events |= POLLWRNORM;
    native[i] = {static_cast<SOCKET>(descriptors[i].fd), events, 0};
  }
  if (!count) { Sleep(timeout < 0 ? INFINITE : static_cast<DWORD>(timeout)); return 0; }
  const int result = WSAPoll(native.data(), static_cast<ULONG>(count), timeout);
  if (result == SOCKET_ERROR) return SocketError();
  for (size_t i = 0; i < count; ++i) {
    const auto events = native[i].revents;
    descriptors[i].revents = ((events & (POLLRDNORM | POLLRDBAND)) ? 1 : 0) |
        ((events & POLLPRI) ? 2 : 0) | ((events & POLLWRNORM) ? 4 : 0) |
        ((events & POLLERR) ? 8 : 0) | ((events & POLLHUP) ? 16 : 0) | ((events & POLLNVAL) ? 32 : 0);
  }
  return result;
}
}
#define MSG_DONTWAIT uwp::kDontWait
#define pollfd uwp::PollFd
#define poll(...) uwp::Poll(__VA_ARGS__)
#undef POLLIN
#define POLLIN 1
