// A daemon socket can be between accept and its FD_CLOEXEC setup (#1255).
module;
#include <spawn.h>

module ycxx.build.action:spawn;

namespace ycxx::build {

int isolate_spawn(posix_spawn_file_actions_t&, short& flags) {
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
  return 0;
}

}  // namespace ycxx::build
