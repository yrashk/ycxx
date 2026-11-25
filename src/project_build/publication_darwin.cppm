module;
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>

module ycxx.build.publication:os;

import std;

namespace ycxx::build {

std::error_code move_publication(const std::filesystem::path& from,
                                 const std::filesystem::path& to, bool exchange) {
  const auto flag = exchange ? RENAME_SWAP : RENAME_EXCL;
  if (::renameatx_np(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), flag) != 0)
    return {errno, std::generic_category()};
  return {};
}

std::expected<bool, std::error_code> same_publication_mount(const std::filesystem::path& parent,
                                                           const std::filesystem::path& child) {
  struct statfs parent_status{};
  struct statfs child_status{};
  if (::statfs(parent.c_str(), &parent_status) != 0 || ::statfs(child.c_str(), &child_status) != 0)
    return std::unexpected(std::error_code(errno, std::generic_category()));
  return parent_status.f_fsid.val[0] == child_status.f_fsid.val[0] &&
         parent_status.f_fsid.val[1] == child_status.f_fsid.val[1] &&
         std::strcmp(parent_status.f_mntonname, child_status.f_mntonname) == 0;
}

}  // namespace ycxx::build
