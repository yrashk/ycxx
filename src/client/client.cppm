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
import ycxx.trace;

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

// Splits a response file's text into arguments: whitespace separates them, and quotes and
// backslashes escape as in a shell's words.
std::vector<std::string> split_words(std::string_view text) {
  std::vector<std::string> out;
  std::string cur;
  bool in_word = false;
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (quote) {
      if (c == quote) quote = 0;
      else if (c == '\\' && quote == '"' && i + 1 < text.size()) cur += text[++i];
      else cur += c;
    } else if (c == '\'' || c == '"') {
      quote = c;
      in_word = true;
    } else if (c == '\\' && i + 1 < text.size()) {
      cur += text[++i];
      in_word = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (in_word) out.push_back(std::exchange(cur, {}));
      in_word = false;
    } else {
      cur += c;
      in_word = true;
    }
  }
  if (in_word) out.push_back(cur);
  return out;
}

// Expands `@file` arguments, relative to the client's working directory (§3.2). A file that
// cannot be read stays an argument, as other compilers keep it.
void expand(std::string_view a, std::vector<std::string>& out, int depth) {
  if (a.size() > 1 && a.starts_with('@') && depth < 16) {
    std::ifstream in(std::string(a.substr(1)), std::ios::binary);
    if (in) {
      std::string text{std::istreambuf_iterator<char>(in), {}};
      for (const std::string& w : split_words(text)) expand(w, out, depth + 1);
      return;
    }
  }
  out.emplace_back(a);
}

std::string cwd_now() {
  std::error_code ec;
  return std::filesystem::current_path(ec).native_encoded_string();
}

// `--trace-engine[=stream|interactive]` (#245): what the request's engine did, as an indented
// log on stderr as it happens, or browsed after the compile.
enum class TraceMode { off, stream, interactive };

std::optional<TraceMode> trace_mode_named(std::string_view a) {
  if (a == "--trace-engine" || a == "--trace-engine=stream") return TraceMode::stream;
  if (a == "--trace-engine=interactive") return TraceMode::interactive;
  return std::nullopt;
}

// `YCXX_TRACE_ENGINE=stream|interactive` (or `1`): the flag for builds whose command lines a
// build system writes. The flag wins.
TraceMode trace_mode_from_environment() {
  const char* e = std::getenv("YCXX_TRACE_ENGINE");
  const std::string_view v = e ? e : "";
  if (falsy(v)) return TraceMode::off;
  if (v == "interactive") return TraceMode::interactive;
  if (v != "stream" && v != "1" && v != "on")
    std::println(std::cerr, "ycxx: warning: YCXX_TRACE_ENGINE='{}' is not one of stream, interactive, 0: streaming", v);
  return TraceMode::stream;
}

// Where a request's trace goes: printed batch by batch, or gathered for the browser.
class TraceOut {
 public:
  explicit TraceOut(TraceMode mode) : mode_(mode), stream_(std::cerr, {trace::colors_for(2), true}) {}

  void events(std::span<const trace::Event> batch) {
    if (mode_ == TraceMode::stream) stream_.batch(batch);
    else tree_.add(batch);
  }
  // From the daemon: times on its clock, made relative to the request's start.
  void batch(daemon::TraceBatch&& b) {
    if (b.end) {
      total_ = b.ended_ns - std::min(b.ended_ns, b.started_ns);
      return;
    }
    trace::rebase(b.events, b.started_ns);
    events(b.events);
  }
  void total(std::uint64_t ns) { total_ = ns; }

  // After the compile: the stream's summary, or the browser (the stream, without a terminal).
  void finish() {
    if (mode_ == TraceMode::interactive) {
      if (trace::browse(tree_)) return;
      std::println(std::cerr, "ycxx: no terminal for --trace-engine=interactive; printing the trace");
      std::vector<trace::Event> all;
      for (const auto& n : tree_.nodes()) all.push_back(n.ev);
      stream_.batch(all);
    }
    stream_.summary(total_);
  }

 private:
  TraceMode mode_;
  trace::Stream stream_;
  trace::Tree tree_;
  std::optional<std::uint64_t> total_;
};

// The request through the daemon; why not, if it could not be answered there. A daemon that
// closed the connection without an answer is started again and asked once more (§7.2).
std::expected<compile::Done, std::string> through_daemon(const compile::Request& r, bool verbose, TraceOut* traced) {
  std::string root = daemon::root_for(compile::first_input(r.argv), r.cwd);
  std::string why;
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto c = daemon::reach(root, true);
    if (!c) return std::unexpected(c.error());
    auto d = daemon::request_compile(*c, r, [&](daemon::TraceBatch&& b) {
      if (traced) traced->batch(std::move(b));
    });
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
// `--trace-engine[=stream|interactive]` shows what went through the engine (#245); through the
// daemon, so a second run shows what its warm engine reused.
int cxx(std::span<const std::string_view> args) {
  Use use = use_from_environment();
  const char* no = std::getenv("YCXX_NO_DAEMON");
  bool forced_off = no && !falsy(no);
  bool verbose = false;
  TraceMode trace_mode = trace_mode_from_environment();
  std::vector<std::string> argv;
  for (std::string_view a : args) {
    if (auto m = trace_mode_named(a)) trace_mode = *m;
    else if (a.starts_with("--trace-engine=")) {
      std::println(std::cerr, "ycxx: error: unknown trace mode in '{}' (stream or interactive)", a);
      return 1;
    } else if (a == "--no-daemon") forced_off = true;
    else if (a == "--daemon") use = use == Use::require ? use : Use::on;
    else if (a == "--daemon=require") use = Use::require;
    else if (a == "-v") verbose = true;
    else expand(a, argv, 0);
  }
  if (forced_off) use = Use::off;

  compile::Request r{std::move(argv), cwd_now(), compile::environment_now(),
                     std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
  std::optional<TraceOut> traced;
  if (trace_mode != TraceMode::off) {
    traced.emplace(trace_mode);
    // The browser shows values; the stream does not, and skips rendering them.
    r.trace = trace_mode == TraceMode::interactive ? compile::Trace::values : compile::Trace::events;
  }
  const auto finish = [&](compile::Done& d) {
    const int status = compile::finish(d);
    if (traced) traced->finish();
    return status;
  };
  if (use != Use::off) {
    auto d = through_daemon(r, verbose, traced ? &*traced : nullptr);
    if (d) return finish(*d);
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
  std::optional<trace::Collector> collector;
  const auto start = std::chrono::steady_clock::now();
  if (traced) collector.emplace([&](std::span<const trace::Event> batch) { traced->events(batch); }, r.trace == compile::Trace::values, start);
  compile::Done d = compile::run(r, disk, session, {}, stopped, {}, collector ? &*collector : nullptr);
  if (collector) {
    collector->finish();
    traced->total(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
  }
  return finish(d);
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

namespace {

// `ycxx ctl trace` (#245): every request the daemon serves from now on, as a stream on stdout or,
// with `--interactive` on a terminal, a browser that grows as requests come, one node each.
int watch(daemon::Connection& c, bool interactive) {
  if (auto ok = daemon::watch_traces(c, interactive); !ok) {
    std::println(std::cerr, "ycxx: {}", ok.error());
    return 1;
  }
  // Times count from the first request seen, on the daemon's clock.
  std::optional<std::uint64_t> base;
  const auto rebase = [&](daemon::TraceBatch& b) {
    if (!base) base = b.started_ns;
    b.started_ns -= std::min(b.started_ns, *base);
    b.ended_ns -= std::min(b.ended_ns, *base);
    trace::rebase(b.events, *base);
  };
  const auto group_of = [](std::uint64_t request) { return (std::uint64_t{1} << 63) | request; };
  if (interactive && trace::is_terminal(0)) {
    trace::Tree tree;
    std::set<std::uint64_t> seen;
    trace::Feed feed{c.fd.get(), [&] {
                       auto b = daemon::next_trace(c);
                       if (!b) return false;
                       rebase(*b);
                       if (seen.insert(b->request).second) tree.open_group(group_of(b->request), std::format("#{} {}", b->request, b->label), b->started_ns);
                       if (b->end) tree.close_group(group_of(b->request), b->ended_ns);
                       else tree.add(b->events, group_of(b->request));
                       return true;
                     }};
    trace::browse(tree, &feed);
    return 0;
  }
  const trace::Style style{trace::colors_for(1), true};
  std::println("ycxx: watching the daemon of {} (pid {}); ^C to stop", c.welcome.root, c.welcome.daemon_pid);
  std::cout.flush();  // a pipe buffers: whoever reads it knows the watch is attached
  std::map<std::uint64_t, trace::Stream> requests;
  std::uint64_t last = 0;
  while (auto b = daemon::next_trace(c)) {
    rebase(*b);
    auto [it, fresh] = requests.try_emplace(b->request, std::cout, style);
    if (fresh || last != b->request) it->second.note(std::format("── #{} {}", b->request, b->label));
    last = b->request;
    if (b->end) {
      it->second.summary(b->ended_ns - std::min(b->ended_ns, b->started_ns));
      requests.erase(it);
      last = 0;
    } else {
      it->second.batch(b->events);
    }
  }
  std::println("ycxx: the daemon has gone");
  return 0;
}

}  // namespace

// `ycxx ctl status|shutdown|trace [--root DIR]`: asks the daemon of DIR's scope (default: the
// working directory's), never starting one.
int control(std::span<const std::string_view> args) {
  std::optional<std::string> command, root;
  bool interactive = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--root" && i + 1 < args.size()) root = std::string(args[++i]);
    else if (args[i] == "--interactive") interactive = true;
    else if (!command) command = std::string(args[i]);
    else command.reset(), i = args.size();
  }
  if (!command || (*command != "status" && *command != "shutdown" && *command != "trace")) {
    std::println(std::cerr, "usage: ycxx ctl status|shutdown|trace [--root DIR] [--interactive]");
    return 2;
  }
  std::string scope = root ? daemon::real_path(*root).value_or(*root) : daemon::root_for(std::nullopt, cwd_now());
  auto c = daemon::reach(scope, false);
  if (!c) {
    std::println(std::cerr, "ycxx: no daemon for {}: {}", scope, c.error());
    return 1;
  }
  if (*command == "trace") return watch(*c, interactive);
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
