// The daemon's clients (DECISIONS §4.7): a c++-compatible command line, an LSP server and a
// control interface. Each will connect to the daemon, starting it if needed. Not built yet.
export module ycxx.client;

import std;

export namespace ycxx::client {
int cxx(std::span<const std::string_view> args);
int lsp(std::span<const std::string_view> args);
int control(std::span<const std::string_view> args);
}

namespace ycxx::client {
namespace {
int not_yet(std::string_view front) {
  std::println(std::cerr, "ycxx: the {} front is not implemented yet", front);
  return 2;
}
}  // namespace

int cxx(std::span<const std::string_view>) { return not_yet("c++"); }
int lsp(std::span<const std::string_view>) { return not_yet("lsp"); }
int control(std::span<const std::string_view>) { return not_yet("ctl"); }
}  // namespace ycxx::client
