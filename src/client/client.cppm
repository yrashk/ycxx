// The daemon's clients (DECISIONS §4.7, daemon design §7.1): a c++-compatible command line, an
// LSP server and a control interface. `cxx` sends its request to the scope's daemon, starting it if
// needed, and runs the same request in-process on any failure; both paths run `compile::run`, so
// stdout, stderr, the exit status and the outputs are the same.
export module ycxx.client;

import std;
import ycxx.compile;
import ycxx.daemon;
import ycxx.engine;
import ycxx.pipeline;
import ycxx.pp;

export namespace ycxx::client {
int cxx(std::span<const std::string_view> args);
int lsp(std::span<const std::string_view> args);
int control(std::span<const std::string_view> args);
}

namespace ycxx::client {
namespace {

// Whether `cxx` uses the daemon: by default, since requests run concurrently in it (#139), with
// an in-process run as the fallback on any failure (daemon design §7.2). `YCXX_DAEMON=0` (or `off`,
// `no`, `false`) turns it off, `YCXX_DAEMON=require` or `--daemon=require` (the tests' hook) make a
// failure to use it an error instead of an in-process run, and `YCXX_NO_DAEMON` set to anything
// but those, or `--no-daemon`, turns it off whatever else says.
enum class Use { off, on, require };

// A value that means "no": unset, empty, `0`, `off`, `no`, `false`.
bool falsy(std::string_view v) { return v.empty() || v == "0" || v == "off" || v == "no" || v == "false"; }

Use use_from_environment() {
  if (const char* e = std::getenv("YCXX_NO_DAEMON"); e && !falsy(e)) return Use::off;
  const char* e = std::getenv("YCXX_DAEMON");
  std::string_view v = e ? e : "";
  if (v == "require") return Use::require;
  if (e && *e && falsy(v)) return Use::off;
  if (!v.empty() && v != "1" && v != "on" && v != "yes" && v != "true")
    std::println(std::cerr, "ycxx: warning: YCXX_DAEMON='{}' is not one of 0, off, 1, on, require: using the daemon", v);
  return Use::on;
}

// Expands `@file` arguments, relative to the client's working directory (§3.2). A file that
// cannot be read stays an argument, as other compilers keep it.
void expand(std::string_view a, std::vector<std::string>& out, int depth) {
  if (a.size() > 1 && a.starts_with('@') && depth < 16) {
    std::ifstream in(std::string(a.substr(1)), std::ios::binary);
    if (in) {
      std::string text{std::istreambuf_iterator<char>(in), {}};
      for (const std::string& w : compile::split_words(text)) expand(w, out, depth + 1);
      return;
    }
  }
  out.emplace_back(a);
}

std::string cwd_now() {
  std::error_code ec;
  return std::filesystem::current_path(ec).native_encoded_string();
}

// The request through the daemon; why not, if it could not be answered there. A daemon that
// closed the connection without an answer is started again and asked once more (§7.2).
std::expected<compile::Done, std::string> through_daemon(const compile::Request& r, bool verbose) {
  std::string root = daemon::root_for(compile::first_input(r.argv), r.cwd);
  std::string why;
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto c = daemon::reach(root, true);
    if (!c) return std::unexpected(c.error());
    auto d = daemon::request_compile(*c, r);
    if (d) {
      if (verbose)
        std::println(std::cerr, "ycxx: answered by daemon {} for {} at revision {} ({} restarts, {} pp regions reused)",
                     c->welcome.daemon_pid, c->welcome.root, d->report.revision, d->report.restarts, d->report.regions_reused);
      return std::move(*d);
    }
    why = d.error().why;
    if (!d.error().lost) break;
  }
  return std::unexpected(why);
}

}  // namespace

// The c++-compatible front (compile.cppm has the options). Hidden options for the daemon:
// `--daemon`, `--daemon=require`, `--no-daemon`; `-v` says whether and where the daemon answered.
int cxx(std::span<const std::string_view> args) {
  Use use = use_from_environment();
  const char* no = std::getenv("YCXX_NO_DAEMON");
  bool forced_off = no && !falsy(no);
  bool verbose = false;
  std::vector<std::string> argv;
  for (std::string_view a : args) {
    if (a == "--no-daemon") forced_off = true;
    else if (a == "--daemon") use = use == Use::require ? use : Use::on;
    else if (a == "--daemon=require") use = Use::require;
    else if (a == "-v") verbose = true;
    else expand(a, argv, 0);
  }
  if (forced_off) use = Use::off;

  compile::Request r{std::move(argv), cwd_now(), compile::environment_now(),
                     std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
  if (use != Use::off) {
    auto d = through_daemon(r, verbose);
    if (d) return compile::finish(*d);
    if (use == Use::require) {
      std::println(std::cerr, "ycxx: error: the daemon is required (YCXX_DAEMON=require) but failed: {}", d.error());
      return 2;
    }
    if (verbose) std::println(std::cerr, "ycxx: running in-process: {}", d.error());
  }
  pp::DiskHost disk;
  const compile::Threads t = compile::threads();
  engine::Pool pool(t.count, t.chaos_seed);
  pipeline::Session session(pool);
  bool stopped = false;
  compile::Done d = compile::run(r, disk, session, {}, stopped);
  return compile::finish(d);
}

// `ycxx lsp [--root DIR]`: the editor's language server, which runs in the daemon of DIR's scope
// (default: the working directory's), started if none runs; this process only copies bytes between
// its stdio and the daemon (daemon design §6.4).
int lsp(std::span<const std::string_view> args) {
  std::optional<std::string> root;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--root" && i + 1 < args.size()) root = std::string(args[++i]);
    else {
      std::println(std::cerr, "ycxx: error: unknown argument '{}' (usage: ycxx lsp [--root DIR])", args[i]);
      return 2;
    }
  }
  auto c = daemon::reach(daemon::root_for(std::nullopt, root ? *root : cwd_now()), true);
  if (!c) {
    std::println(std::cerr, "ycxx: error: cannot reach the daemon: {}", c.error());
    return 1;
  }
  auto status = daemon::tunnel_lsp(*c, 0, 1);
  if (!status) {
    std::println(std::cerr, "ycxx: error: {}", status.error());
    return 1;
  }
  return *status;
}

// `ycxx ctl status|shutdown [--root DIR]`: asks the daemon of DIR's scope (default: the working
// directory's), never starting one.
int control(std::span<const std::string_view> args) {
  std::optional<std::string> command, root;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--root" && i + 1 < args.size()) root = std::string(args[++i]);
    else if (!command) command = std::string(args[i]);
    else command.reset(), i = args.size();
  }
  if (!command || (*command != "status" && *command != "shutdown")) {
    std::println(std::cerr, "usage: ycxx ctl status|shutdown [--root DIR]");
    return 2;
  }
  std::string scope = root ? daemon::real_path(*root).value_or(*root) : daemon::root_for(std::nullopt, cwd_now());
  auto c = daemon::reach(scope, false);
  if (!c) {
    std::println(std::cerr, "ycxx: no daemon for {}: {}", scope, c.error());
    return 1;
  }
  if (*command == "shutdown") {
    auto ok = daemon::request_shutdown(*c);
    if (!ok) std::println(std::cerr, "ycxx: {}", ok.error());
    return ok ? 0 : 1;
  }
  auto s = daemon::request_status(*c);
  if (!s) {
    std::println(std::cerr, "ycxx: {}", s.error());
    return 1;
  }
  // One `name: value` line per field of the status, by reflection.
  template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^daemon::Status,
                                                                                                  std::meta::access_context::unchecked()))) {
    std::println("{}: {}", std::meta::identifier_of(m), s->[:m:]);
  }
  return 0;
}

}  // namespace ycxx::client
