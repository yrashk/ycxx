// The resident daemon (DECISIONS §4.1, §4.7): owns the query engine and serves the clients.
// Not built yet; the engine starts with #14.
export module ycxx.daemon;

import std;

export namespace ycxx::daemon {
int run(std::span<const std::string_view> args);
}

namespace ycxx::daemon {
int run(std::span<const std::string_view>) {
  std::println(std::cerr, "ycxx: the daemon is not implemented yet");
  return 2;
}
}  // namespace ycxx::daemon
