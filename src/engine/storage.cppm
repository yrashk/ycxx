module;

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

export module ycxx.engine:storage;

import std;
import ycxx.hash;
export import :storage_os;

export namespace ycxx::engine {

// Available on local filesystems that support preallocation. Heap storage is the default.
class ArenaBacking {
public:
  explicit ArenaBacking(std::string directory) : directory_(std::move(directory)) {}
  void* allocate(std::size_t bytes, std::size_t alignment) const noexcept;
  static void release(void* base, std::size_t bytes) noexcept { (void)munmap(base, bytes); }

private:
  std::string directory_;
};

// An immutable segment's ownership and address. Bytes are kept separate from their encoding.
class SnapshotMapping {
public:
  explicit SnapshotMapping(const std::string& path) noexcept;
  ~SnapshotMapping();
  SnapshotMapping(const SnapshotMapping&) = delete;
  SnapshotMapping& operator=(const SnapshotMapping&) = delete;
  std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
  std::span<const std::byte> bytes_;
};

// Atomically publish a complete segment. Existing mappings keep their old inode across a rename.
bool write_snapshot(const std::string& path, std::span<const std::byte> bytes) noexcept;

// The running executable's identity, independent of its installation path.
std::optional<Fp128> snapshot_build_id() noexcept;

}  // namespace ycxx::engine

namespace ycxx::engine {

void* ArenaBacking::allocate(std::size_t bytes, std::size_t alignment) const noexcept {
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0 || static_cast<std::size_t>(page) % alignment != 0 ||
      bytes > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) return nullptr;
  try {
    std::string path = directory_ + "/ycxx-arena.XXXXXX";
    const int fd = storage_os::temp_file(path);
    if (fd < 0) return nullptr;
    if (unlink(path.c_str()) != 0) {
      close(fd);
      return nullptr;
    }
    void* base = MAP_FAILED;
    if (storage_os::reserve_file(fd, static_cast<off_t>(bytes))) {
      base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    close(fd);
    return base == MAP_FAILED ? nullptr : base;
  } catch (...) {
    return nullptr;
  }
}

SnapshotMapping::SnapshotMapping(const std::string& path) noexcept {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) return;
  struct stat status {};
  if (fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_size > 0 &&
      static_cast<std::uintmax_t>(status.st_size) <= std::numeric_limits<std::size_t>::max()) {
    const auto size = static_cast<std::size_t>(status.st_size);
    const void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (base != MAP_FAILED) bytes_ = {static_cast<const std::byte*>(base), size};
  }
  close(fd);
}

SnapshotMapping::~SnapshotMapping() {
  if (!bytes_.empty()) (void)munmap(const_cast<std::byte*>(bytes_.data()), bytes_.size());
}

std::optional<Fp128> snapshot_build_id() noexcept {
  static const auto build = storage_os::running_build_id();
  return build;
}

bool write_snapshot(const std::string& path, std::span<const std::byte> bytes) noexcept {
  try {
    std::string staged = path + ".XXXXXX";
    const int fd = storage_os::temp_file(staged);
    if (fd < 0) return false;
    while (!bytes.empty()) {
      const auto length = std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
      const auto written = write(fd, bytes.data(), length);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) break;
      bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    const bool complete = bytes.empty() && fchmod(fd, 0444) == 0 && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    const bool published = complete && closed && std::rename(staged.c_str(), path.c_str()) == 0;
    if (!published) (void)unlink(staged.c_str());
    return published;
  } catch (...) {
    return false;
  }
}

}  // namespace ycxx::engine
