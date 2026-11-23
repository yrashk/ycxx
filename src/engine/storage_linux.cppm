module;

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

export module ycxx.engine:storage_os;

import std;
import ycxx.hash;

export namespace ycxx::engine::storage_os {

bool reserve_file(int fd, off_t bytes) noexcept { return posix_fallocate(fd, 0, bytes) == 0; }

int temp_file(std::string& path) noexcept { return mkostemp(path.data(), O_CLOEXEC); }

std::optional<Fp128> running_build_id() noexcept {
  const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  Hasher hash;
  std::array<std::byte, 64 * 1024> buffer;
  ssize_t n;
  do {
    n = read(fd, buffer.data(), buffer.size());
    if (n > 0) hash.bytes(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)));
  } while (n > 0 || (n < 0 && errno == EINTR));
  close(fd);
  if (n != 0) return std::nullopt;
  return hash.finish();
}

}  // namespace ycxx::engine::storage_os
