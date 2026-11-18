// What differs on macOS (daemon design §3.1, §7.1): the executable's path, the peer's uid, no
// SIGPIPE on a socket, stat times and the runtime directory. CMake picks this file or
// os_linux.cppm.
module;
#include <dirent.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
export module ycxx.daemon:os;

import std;

export namespace ycxx::daemon {

inline constexpr std::string_view os_name = "macos";

std::string executable_path() {
  std::uint32_t size = 0;
  ::_NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (::_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  buf.resize(std::strlen(buf.c_str()));
  if (char* real = ::realpath(buf.c_str(), nullptr)) {
    buf = real;
    ::free(real);
  }
  return buf;
}

std::optional<std::uint32_t> peer_uid(int fd) {
  uid_t uid = 0;
  gid_t gid = 0;
  if (::getpeereid(fd, &uid, &gid) != 0) return std::nullopt;
  return uid;
}

void no_sigpipe(int fd) {
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}

std::int64_t mtime_ns(const struct stat& st) { return std::int64_t(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec; }
std::int64_t ctime_ns(const struct stat& st) { return std::int64_t(st.st_ctimespec.tv_sec) * 1'000'000'000 + st.st_ctimespec.tv_nsec; }

// `$TMPDIR/ycxx-<uid>` (§3.1).
std::string default_runtime_dir() {
  const char* tmp = std::getenv("TMPDIR");
  std::string base = tmp && *tmp ? tmp : "/tmp";
  while (base.size() > 1 && base.ends_with('/')) base.pop_back();
  return std::format("{}/ycxx-{}", base, ::getuid());
}

// Closes every descriptor from `lowest` on, in a child after fork (§7.1).
void close_fds_from(int lowest) {
  std::vector<int> open;
  if (DIR* d = ::opendir("/dev/fd")) {
    while (dirent* e = ::readdir(d)) {
      int fd = std::atoi(e->d_name);
      if (fd >= lowest && e->d_name[0] != '.') open.push_back(fd);
    }
    ::closedir(d);
  }
  for (int fd : open) ::close(fd);
}

}  // namespace ycxx::daemon
