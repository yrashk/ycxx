module;

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <unistd.h>

export module ycxx.engine:storage_os;

import std;

export namespace ycxx::engine::storage_os {

bool reserve_file(int fd, off_t bytes) noexcept {
  fstore_t request{F_ALLOCATEALL, F_PEOFPOSMODE, 0, bytes, 0};
  return fcntl(fd, F_PREALLOCATE, &request) == 0 && ftruncate(fd, bytes) == 0;
}

int temp_file(std::string& path) noexcept {
  const int fd = mkstemp(path.data());
  if (fd >= 0) (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

int open_executable() noexcept {
  std::array<char, 4096> path{};
  auto size = static_cast<std::uint32_t>(path.size());
  if (_NSGetExecutablePath(path.data(), &size) != 0) return -1;
  return open(path.data(), O_RDONLY | O_CLOEXEC);
}

}  // namespace ycxx::engine::storage_os
