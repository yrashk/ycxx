#include <csignal>

extern "C" int direct_breakpoint();
extern "C" int guarded_breakpoint(int selected);

namespace {
  volatile std::sig_atomic_t hits = 0;

  void on_trap(int signal) {
    if (signal == SIGTRAP) hits = hits + 1;
  }
}

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  const char mode = argv[1][0];
  struct sigaction action {};
  action.sa_handler = mode == 'h' ? on_trap : SIG_DFL;
  if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGTRAP, &action, nullptr) != 0) return 2;
  sigset_t signals;
  if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGTRAP) != 0 ||
      sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0) return 3;

  switch (mode) {
    case 'i': return guarded_breakpoint(0) != 17;
    case 'd': return direct_breakpoint();
    case 's': return guarded_breakpoint(1);
    case 'h':
      if (guarded_breakpoint(0) != 17 || hits != 0) return 4;
      if (direct_breakpoint() != 13 || hits != 1) return 5;
      if (guarded_breakpoint(1) != 17 || hits != 2) return 6;
      return 0;
    default: return 7;
  }
}
