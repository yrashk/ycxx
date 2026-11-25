// The command line of the one ycxx binary (DECISIONS §4.7): `ycxx <front> [args...]` picks the
// daemon or one of its clients. The fronts are the enumerators of `front`; their names on the
// command line come from reflection.
export module ycxx.driver;

import std;
import ycxx.build.rule_worker;
import ycxx.version;
import ycxx.daemon;
import ycxx.client;
import ycxx.driver.archive;

namespace ycxx::driver {

enum class front { daemon, cxx, build, show, lsp, ctl, archive };

int run_front(front f, std::span<const std::string_view> args) {
  switch (f) {
    case front::daemon: return daemon::run(args);
    case front::cxx: return client::cxx(args);
    case front::build: return client::build(args);
    case front::show: return client::show(args);
    case front::lsp: return client::lsp(args);
    case front::ctl: return client::control(args);
    case front::archive: return run_archive(args);
  }
  std::unreachable();
}

std::optional<front> front_named(std::string_view name) {
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^front))) {
    if (name == std::meta::identifier_of(e)) return [:e:];
  }
  return std::nullopt;
}

void usage(std::ostream& out) {
  std::print(out, "usage: ycxx --version | ycxx <front> [args...]\nfronts:");
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^front))) {
    std::print(out, " {}", std::meta::identifier_of(e));
  }
  std::println(out);
}

export int run(std::span<const std::string_view> argv)
  pre(!argv.empty())
{
  auto args = argv.subspan(1);
  if (args.empty()) {
    usage(std::cerr);
    return 2;
  }
  if (args[0] == "--internal-rule-worker") return build::run_rule_worker(args.subspan(1));
  if (args[0] == "--version") {
    std::println("ycxx {}", version);
    return 0;
  }
  if (args[0] == "--help") {
    usage(std::cout);
    return 0;
  }
  if (auto f = front_named(args[0])) return run_front(*f, args.subspan(1));
  // `ycxx -fsyntax-only a.cpp`: an option first means the c++-compatible front.
  if (args[0].starts_with("-")) return run_front(front::cxx, args);
  std::println(std::cerr, "ycxx: unknown front '{}'", args[0]);
  usage(std::cerr);
  return 2;
}

}  // namespace ycxx::driver
