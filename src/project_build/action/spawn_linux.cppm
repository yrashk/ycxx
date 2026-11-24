// A daemon socket can be between accept and its FD_CLOEXEC setup (#1255).
module;
#include <spawn.h>
#include <unistd.h>

module ycxx.build.action:spawn;

namespace ycxx::build {

int isolate_spawn(posix_spawn_file_actions_t& files, short&) {
  return ::posix_spawn_file_actions_addclosefrom_np(&files, STDERR_FILENO + 1);
}

}  // namespace ycxx::build
