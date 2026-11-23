module;

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

export module ycxx.engine:storage_os;

import std;

export namespace ycxx::engine::storage_os {

bool reserve_file(int fd, off_t bytes) noexcept { return posix_fallocate(fd, 0, bytes) == 0; }

int temp_file(std::string& path) noexcept { return mkostemp(path.data(), O_CLOEXEC); }

int open_executable() noexcept { return open("/proc/self/exe", O_RDONLY | O_CLOEXEC); }

}  // namespace ycxx::engine::storage_os
