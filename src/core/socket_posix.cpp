#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

namespace {
// Guest ioctlsocket uses the Winsock command values, and its argument is a
// big-endian u_long in guest memory. Neither matches a POSIX ioctl, so passing
// them through fails with ENOTTY.
constexpr uint32_t kWinsockFIONBIO = 0x8004667E;
constexpr uint32_t kWinsockFIONREAD = 0x4004667F;
}  // namespace

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  int fd = static_cast<int>(handle);
  switch (cmd) {
    case kWinsockFIONBIO: {
      uint32_t enable;
      std::memcpy(&enable, arg, sizeof(enable));  // only zero/non-zero matters
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) {
        return -1;
      }
      flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
      return fcntl(fd, F_SETFL, flags) < 0 ? -1 : 0;
    }
    case kWinsockFIONREAD: {
      int available = 0;
      if (ioctl(fd, FIONREAD, &available) < 0) {
        return -1;
      }
      uint32_t be = __builtin_bswap32(static_cast<uint32_t>(available));
      std::memcpy(arg, &be, sizeof(be));
      return 0;
    }
    default:
      return ioctl(fd, cmd, arg);
  }
}

}  // namespace rex::net
