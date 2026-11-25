module;
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

module ycxx.build.publication:os;

import std;

namespace ycxx::build {

std::error_code move_publication(const std::filesystem::path& from,
                                 const std::filesystem::path& to, bool exchange) {
  const auto flag = exchange ? RENAME_EXCHANGE : RENAME_NOREPLACE;
  if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), flag) != 0)
    return {errno, std::generic_category()};
  return {};
}

std::expected<bool, std::error_code> same_publication_mount(const std::filesystem::path& parent,
                                                           const std::filesystem::path& child) {
  // A bind mount can share a device number with its parent.
  constexpr int flags = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT;
  struct statx parent_status{};
  struct statx child_status{};
  if (::statx(AT_FDCWD, parent.c_str(), flags, STATX_MNT_ID, &parent_status) != 0 ||
      ::statx(AT_FDCWD, child.c_str(), flags, STATX_MNT_ID, &child_status) != 0)
    return std::unexpected(std::error_code(errno, std::generic_category()));
  if ((parent_status.stx_mask & child_status.stx_mask & STATX_MNT_ID) == 0)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  return parent_status.stx_mnt_id == child_status.stx_mnt_id;
}

}  // namespace ycxx::build
