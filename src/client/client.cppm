// The daemon's clients (DECISIONS §4.7): a c++-compatible command line, an LSP server and a
// control interface. Each will connect to the daemon, starting it if needed. Not built yet.
export module ycxx.client;

import std;
import ycxx.parse;

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

// The c++-compatible front. For now it runs in-process and does only `-fsyntax-only` (#41); the
// pipeline to an executable is #43's.
int cxx(std::span<const std::string_view> args) {
  bool syntax_only = false;
  std::vector<std::string_view> files;
  for (std::string_view a : args) {
    if (a == "-fsyntax-only") syntax_only = true;
    else if (a.starts_with("-std=") || a == "-pedantic-errors") continue;
    else if (a.starts_with("-")) {
      std::println(std::cerr, "ycxx: unsupported option '{}'", a);
      return 2;
    } else files.push_back(a);
  }
  if (!syntax_only) return not_yet("c++ (only -fsyntax-only)");
  if (files.empty()) {
    std::println(std::cerr, "ycxx: no input files");
    return 2;
  }
  int errors = 0;
  for (std::string_view f : files) {
    std::ifstream in{std::string(f), std::ios::binary};
    if (!in) {
      std::println(std::cerr, "ycxx: cannot open '{}'", f);
      return 2;
    }
    std::string text{std::istreambuf_iterator<char>(in), {}};
    errors += parse::check_file(f, text, std::cerr);
  }
  return errors ? 1 : 0;
}
int lsp(std::span<const std::string_view>) { return not_yet("lsp"); }
int control(std::span<const std::string_view>) { return not_yet("ctl"); }
}  // namespace ycxx::client
