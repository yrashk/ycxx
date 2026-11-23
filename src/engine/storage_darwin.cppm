module;

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdlib.h>
#include <unistd.h>

export module ycxx.engine:storage_os;

import std;
import ycxx.hash;

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

std::optional<Fp128> running_build_id() noexcept {
  // dyld's first image is the executable. Its linker-supplied UUID lives in the loaded
  // header even after the installation path is atomically replaced.
  const auto* header = _dyld_get_image_header(0);
  if (!header || header->magic != MH_MAGIC_64 || header->filetype != MH_EXECUTE) return std::nullopt;
  std::span commands(reinterpret_cast<const std::byte*>(header) + sizeof(mach_header_64), header->sizeofcmds);
  std::optional<Fp128> identity;
  for (std::uint32_t i = 0; i < header->ncmds; ++i) {
    if (commands.size() < sizeof(load_command)) return std::nullopt;
    load_command command{};
    std::memcpy(&command, commands.data(), sizeof(command));
    if (command.cmdsize < sizeof(command) || command.cmdsize > commands.size()) return std::nullopt;
    if (command.cmd == LC_UUID) {
      if (identity || command.cmdsize != sizeof(uuid_command)) return std::nullopt;
      uuid_command uuid{};
      std::memcpy(&uuid, commands.data(), sizeof(uuid));
      if (std::ranges::all_of(uuid.uuid, [](auto byte) { return byte == 0; })) return std::nullopt;
      identity = fingerprint128(std::as_bytes(std::span(uuid.uuid)));
    }
    commands = commands.subspan(command.cmdsize);
  }
  return commands.empty() ? identity : std::nullopt;
}

}  // namespace ycxx::engine::storage_os
