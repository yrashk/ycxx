// What differs on Linux (daemon design §3.1, §7.1): the executable's path, the peer's uid, no
// SIGPIPE on a socket, stat times and the runtime directory. CMake picks this file or
// os_darwin.cppm.
module;
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
export module ycxx.daemon:os;

import std;

export namespace ycxx::daemon {

// The flags of a send(2) that must not raise SIGPIPE.
inline constexpr int send_flags = MSG_NOSIGNAL;

inline constexpr std::string_view os_name = "linux";

std::string executable_path() {
  std::error_code ec;
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.native_encoded_string();
}

std::optional<std::uint32_t> peer_uid(int fd) {
  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return std::nullopt;
  return cred.uid;
}

// Linux has no per-socket switch: every write to a socket passes `send_flags` (write_all).
void no_sigpipe(int) {}

std::int64_t mtime_ns(const struct stat& st) { return std::int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec; }
std::int64_t ctime_ns(const struct stat& st) { return std::int64_t(st.st_ctim.tv_sec) * 1'000'000'000 + st.st_ctim.tv_nsec; }

// `$XDG_RUNTIME_DIR/ycxx`, else `/tmp/ycxx-<uid>` (§3.1).
std::string default_runtime_dir() {
  if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return std::format("{}/ycxx", x);
  return std::format("/tmp/ycxx-{}", ::getuid());
}

// Closes every descriptor from `lowest` on, in a child after fork (§7.1).
void close_fds_from(int lowest) {
  if (::syscall(SYS_close_range, static_cast<unsigned>(lowest), ~0u, 0u) == 0) return;
  for (long fd = lowest, n = ::sysconf(_SC_OPEN_MAX); fd < n && fd < 65536; ++fd) ::close(static_cast<int>(fd));
}

}  // namespace ycxx::daemon
