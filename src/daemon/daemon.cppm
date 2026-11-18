// The resident daemon (DECISIONS §4.1, §4.7, §4.10; docs/design/daemon.md): owns the engine and
// serves the clients over a Unix socket. `ycxx daemon` runs it; clients reach it through :connect.
//
//   :posix :os        system calls (:os differs between macOS and Linux)
//   :scope            the project root, the runtime directory and the socket's name (§2, §3.1)
//   :protocol         messages and framing (§3)
//   :connect          finding, starting and asking the daemon, for clients (§7.1, §7.2)
//   :events :watch    dirty paths and cookies; FSEvents or inotify (§5.4, §5.5)
//   :files            the file table and batches (§5.2, §5.3)
//   :admission        the progress quantum (§4.2)
//   :server           the daemon process (§3, §4, §7)
export module ycxx.daemon;

import std;
export import :posix;
export import :os;
export import :scope;
export import :protocol;
export import :connect;
// Every interface partition is exported ([module.unit]/3); the unit tests use them too.
export import :events;
export import :watch;
export import :files;
export import :admission;
export import :server;

export namespace ycxx::daemon {
// `ycxx daemon [--root DIR] [--idle-exit SECONDS]`: serves the scope of DIR (default: the scope of
// the working directory) until idle for SECONDS (default 1800, or `YCXX_DAEMON_IDLE_EXIT`),
// `ycxx ctl shutdown` or SIGTERM. Clients start it themselves; `--ready-fd N` is theirs.
int run(std::span<const std::string_view> args);
}

namespace ycxx::daemon {

namespace {
std::optional<long long> number(std::string_view s) {
  long long v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size() || v < 0) return std::nullopt;
  return v;
}
}  // namespace

int run(std::span<const std::string_view> args) {
  ServerOptions o;
  if (const char* e = std::getenv("YCXX_DAEMON_IDLE_EXIT"); e && *e)
    if (auto n = number(e)) o.idle_exit = std::chrono::seconds(*n);
  if (const char* e = std::getenv("YCXX_NO_WATCH"); e && std::string_view(e) == "1") o.watch = false;
  std::optional<std::string> root;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    auto value = [&]() -> std::optional<std::string_view> {
      if (i + 1 == args.size()) return std::nullopt;
      return args[++i];
    };
    std::optional<std::string_view> v;
    if (a == "--root" && (v = value())) root = std::string(*v);
    else if (a == "--ready-fd" && (v = value()) && number(*v)) o.ready_fd = static_cast<int>(*number(*v));
    else if (a == "--idle-exit" && (v = value()) && number(*v)) o.idle_exit = std::chrono::seconds(*number(*v));
    else {
      std::println(std::cerr, "usage: ycxx daemon [--root DIR] [--idle-exit SECONDS]");
      return 2;
    }
  }
  std::error_code ec;
  std::string cwd = std::filesystem::current_path(ec).native_encoded_string();
  o.root = root ? real_path(*root).value_or(*root) : root_for(std::nullopt, cwd);
  return serve(o);
}

}  // namespace ycxx::daemon
