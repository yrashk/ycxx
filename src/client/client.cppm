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
enum class TraceMode { off, stream, interactive, json };

std::optional<TraceMode> trace_mode_named(std::string_view a) {
  if (a == "--trace-engine" || a == "--trace-engine=stream") return TraceMode::stream;
  if (a == "--trace-engine=interactive") return TraceMode::interactive;
  if (a == "--trace-engine=json") return TraceMode::json;
  return std::nullopt;
}

// `YCXX_TRACE_ENGINE=stream|interactive` (or `1`): the flag for builds whose command lines a
// build system writes. The flag wins.
TraceMode trace_mode_from_environment() {
  const char* e = std::getenv("YCXX_TRACE_ENGINE");
  const std::string_view v = e ? e : "";
  if (falsy(v)) return TraceMode::off;
  if (v == "interactive") return TraceMode::interactive;
  if (v == "json") return TraceMode::json;
  if (v != "stream" && v != "1" && v != "on")
    std::println(std::cerr, "ycxx: warning: YCXX_TRACE_ENGINE='{}' is not one of stream, interactive, json, 0: streaming", v);
  return TraceMode::stream;
}

// Where a request's trace goes: printed batch by batch, or gathered for the browser.
class TraceOut {
 public:
  explicit TraceOut(TraceMode mode)
      : mode_(mode), stream_(std::cerr, {mode != TraceMode::json && trace::colors_for(2), true, mode == TraceMode::json}) {}

  void events(std::span<const trace::Event> batch) {
    if (mode_ == TraceMode::interactive) tree_.add(batch);
    else stream_.batch(batch);
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
  // A new run of the request (a retry, or the in-process fallback): its trace starts afresh, as
  // event numbers restart in another engine.
  void restart(std::string_view why) {
    if (mode_ == TraceMode::json) std::println(std::cerr, R"({{"restart":{}}})", trace::json_string(why));
    else std::println(std::cerr, "ycxx: {}: tracing it again", why);
    tree_ = {};
    stream_.reset();
    total_.reset();
  }

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
// A connection another thread may cut (`ycxx cxx --watch`'s browser quitting): its descriptor is
// published under `m` while in use, and `stop` shuts it down, so a build in flight or a wait ends.
struct Interrupt {
  std::mutex m;
  int fd = -1;
  bool stopped = false;
  // Publishes `c`'s descriptor; false when already stopped.
  bool enter(int c) {
    std::lock_guard g(m);
    fd = stopped ? -1 : c;
    return !stopped;
  }
  // Before the connection closes, so `stop` never shuts a reused descriptor.
  void leave() {
    std::lock_guard g(m);
    fd = -1;
  }
  void stop() {
    std::lock_guard g(m);
    stopped = true;
    if (fd >= 0) daemon::shutdown_both(fd);
  }
  bool is_stopped() {
    std::lock_guard g(m);
    return stopped;
  }
};

// `on_trace` gets the request's trace batches; `on_retry` is called before asking a new daemon.
// `instance`, when given, is set to the daemon that answered (`Welcome::instance`); `cut`, when given, can interrupt it.
using OnTrace = std::function<void(daemon::TraceBatch&&)>;
std::expected<compile::Done, std::string> through_daemon(const compile::Request& r, bool verbose, const OnTrace& on_trace = {},
                                                         const std::function<void()>& on_retry = {}, std::uint64_t* instance = nullptr,
                                                         Interrupt* cut = nullptr) {
  std::string root = daemon::root_for(compile::first_input(r.argv), r.cwd);
  std::string why;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (cut && cut->is_stopped()) return std::unexpected("stopped");
    if (attempt > 0 && on_retry) on_retry();
    auto c = daemon::reach(root, true);
    if (!c) return std::unexpected(c.error());
    if (cut && !cut->enter(c->fd.get())) return std::unexpected("stopped");
    auto d = daemon::request_compile(*c, r, on_trace);
    if (cut) cut->leave();
    if (d) {
      if (instance) *instance = c->welcome.instance;
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

// `compile::finish` for `--trace-engine=json`, where every stderr line is JSON: outputs written,
// stdout as it is, the diagnostics and exit status as one line.
int finish_json(compile::Done& d) {
  for (const compile::Output& o : d.outputs) {
    if (auto err = pipeline::write_file(o.path, o.bytes, o.executable)) {
      d.err += std::format("ycxx: error: {}\n", *err);
      d.exit_status = 1;
    }
  }
  std::cout << d.out << std::flush;
  std::println(std::cerr, R"({{"exit_status":{},"diagnostics":{}}})", d.exit_status, trace::json_string(d.err));
  return d.exit_status;
}

// Seconds since the epoch: a request's `build_time`.
std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Waits, through the daemon of `root`, until a file some build read changes after revision `since`
// of daemon `instance` (0: no build was answered) (#245): how many batches changed something (0: the daemon stopped, so the next
// build starts a new one), or nothing when it cannot be asked or was interrupted.
std::optional<std::uint64_t> await_change(const std::string& root, std::uint64_t since, std::uint64_t instance, Interrupt& cut) {
  auto c = daemon::reach(root, true);
  if (!c) {
    std::println(std::cerr, "ycxx: error: --watch: cannot reach the daemon: {}", c.error());
    return std::nullopt;
  }
  if (!cut.enter(c->fd.get())) return std::nullopt;
  auto changed = daemon::await_change(*c, since, instance);
  cut.leave();
  if (cut.is_stopped()) return std::nullopt;
  return changed.value_or(0);
}

// `ycxx cxx --watch` (#245): builds, then waits for the daemon to see a file some build read
// change, and builds again, until interrupted. The daemon is what watches the files, so it is
// required. With the stream, each build prints its trace under a header; with the browser, the
// builds run on a thread of their own and each becomes a node of a live tree, holding its
// diagnostics.
int watch_builds(compile::Request r, bool verbose, TraceMode mode) {
  const std::string root = daemon::root_for(compile::first_input(r.argv), r.cwd);
  Interrupt cut;
  const auto build_label = [](std::uint64_t n, std::optional<std::uint64_t> changed) {
    if (n == 1) return std::string("build 1");
    if (!changed || *changed == 0) return std::format("build {}, by a new daemon", n);
    return std::format("build {}, after {} change{}", n, *changed, *changed == 1 ? "" : "s");
  };
  if (mode != TraceMode::interactive || !trace::is_terminal(0)) {
    std::optional<std::uint64_t> changed;
    const TraceMode each = mode == TraceMode::json ? TraceMode::json : TraceMode::stream;
    for (std::uint64_t n = 1;; ++n) {
      if (mode == TraceMode::json) std::println(std::cerr, R"({{"build":{},"label":{}}})", n, trace::json_string(build_label(n, changed)));
      else std::println(std::cerr, "\n── {}", build_label(n, changed));
      r.build_time = now_seconds();
      std::optional<TraceOut> traced;
      if (mode != TraceMode::off) traced.emplace(each);
      std::uint64_t instance = 0;
      std::uint64_t revision = 0;
      auto d = through_daemon(r, verbose, [&](daemon::TraceBatch&& b) {
        if (traced) traced->batch(std::move(b));
      }, [&] {
        if (traced) traced->restart("the daemon was lost; asking a new one");
      }, &instance);
      const bool json = mode == TraceMode::json;
      if (d) {
        revision = d->report.started_at;
        if (json) finish_json(*d);
        else compile::finish(*d);
        if (traced) traced->finish();
      } else if (json) {
        std::println(std::cerr, R"({{"error":{}}})", trace::json_string("the daemon could not build it: " + d.error()));
      } else {
        std::println(std::cerr, "ycxx: error: the daemon could not build it: {}", d.error());
      }
      if (json) std::println(std::cerr, R"({{"watching":"the files it read"}})");
      else std::println(std::cerr, "ycxx: watching the files it read; ^C to stop");
      changed = await_change(root, revision, instance, cut);
      if (!changed) return 1;
    }
  }

  // The browser on this thread, the builds on another; what they find crosses in `items`.
  struct Item {
    enum { trace, done } what = trace;
    std::uint64_t build = 0;
    std::uint64_t space = 0;      // the build's event numbers, apart from every other run's
    std::uint64_t offset_ns = 0;  // the build's start, since the first's, on this process's clock
    daemon::TraceBatch batch;
    std::string label;
    std::string text;  // with `done`: the build's diagnostics and outputs' errors
  };
  struct Shared {
    std::mutex m;
    std::deque<Item> items;
    bool ended = false;
  } shared;
  auto wake = daemon::make_pipe();
  if (!wake) {
    std::println(std::cerr, "ycxx: error: {}", wake.error());
    return 1;
  }
  const int wake_w = wake->second.get();
  const auto post = [&](Item i) {
    {
      std::lock_guard g(shared.m);
      shared.items.push_back(std::move(i));
    }
    char c = 'w';
    daemon::write_all(wake_w, std::as_bytes(std::span(&c, 1)));
  };
  const auto first = std::chrono::steady_clock::now();
  std::jthread builder([&] {
    std::optional<std::uint64_t> changed;
    for (std::uint64_t n = 1; !cut.is_stopped(); ++n) {
      r.build_time = now_seconds();
      const std::string label = build_label(n, changed);
      const auto offset = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - first).count());
      // Event numbers restart in another engine (a retry, a new daemon): each run gets a space.
      std::uint64_t run = n << 2;
      std::uint64_t instance = 0;
      auto d = through_daemon(r, verbose, [&](daemon::TraceBatch&& b) {
        post({Item::trace, n, run, offset, std::move(b), label, {}});
      }, [&] { ++run; }, &instance, &cut);
      Item done{Item::done, n, run, offset, {}, label, {}};
      std::uint64_t revision = 0;
      if (d) {
        revision = d->report.started_at;
        for (const compile::Output& o : d->outputs) {
          if (auto err = pipeline::write_file(o.path, o.bytes, o.executable)) done.text += std::format("ycxx: error: {}\n", *err);
        }
        done.text = d->out + d->err + done.text;
        if (done.text.empty()) done.text = d->exit_status == 0 ? "built, no diagnostics" : "failed";
      } else {
        done.text = "the daemon could not build it: " + d.error();
      }
      post(std::move(done));
      changed = await_change(root, revision, instance, cut);
      if (!changed) break;
    }
    std::lock_guard g(shared.m);
    shared.ended = true;
  });

  trace::Tree tree;
  const auto group_of = [](std::uint64_t build) { return (std::uint64_t{1} << 63) | build; };
  std::set<std::uint64_t> opened;
  const auto open = [&](const Item& i, std::uint64_t start_ns) {
    if (opened.insert(i.build).second) tree.open_group(group_of(i.build), i.label, start_ns);
  };
  trace::Feed feed{wake->first.get(), [&] {
                     daemon::drain(wake->first.get());
                     std::deque<Item> items;
                     bool ended = false;
                     {
                       std::lock_guard g(shared.m);
                       items.swap(shared.items);
                       ended = shared.ended;
                     }
                     for (Item& i : items) {
                       const std::uint64_t id = group_of(i.build);
                       if (i.what == Item::done) {
                         open(i, i.offset_ns);  // a build that sent no trace still shows its diagnostics
                         tree.annotate(id, std::move(i.text));
                         continue;
                       }
                       // Times from the build's own start, placed at its start on this clock; event
                       // numbers in the run's own space.
                       daemon::TraceBatch& b = i.batch;
                       const std::uint64_t start = b.started_ns;
                       trace::rebase(b.events, start);
                       const std::uint64_t space = i.space << 40;
                       for (trace::Event& e : b.events) {
                         e.start_ns += i.offset_ns;
                         e.end_ns += i.offset_ns;
                         e.id += space;
                         if (e.parent != 0) e.parent += space;
                       }
                       open(i, i.offset_ns);
                       if (b.end) tree.close_group(id, i.offset_ns + (b.ended_ns - std::min(b.ended_ns, start)));
                       else tree.add(b.events, id);
                     }
                     return !ended;
                   },
                   std::format("rebuilding on every change of the files it reads ({})", root)};
  trace::browse(tree, &feed);
  // Stops the builder: a build in flight or its wait is cut, and the daemon cancels the build.
  cut.stop();
  return 0;
}

}  // namespace

// The c++-compatible front (compile.cppm has the options). Hidden options for the daemon:
// `--daemon`, `--daemon=require`, `--no-daemon`; `-v` says whether and where the daemon answered.
// `--trace-engine[=stream|interactive]` shows what went through the engine (#245); through the
// daemon, so a second run shows what its warm engine reused. `--watch` builds again whenever a
// file the build read changes.
int cxx(std::span<const std::string_view> args) {
  Use use = use_from_environment();
  const char* no = std::getenv("YCXX_NO_DAEMON");
  bool forced_off = no && !falsy(no);
  bool verbose = false;
  TraceMode trace_mode = trace_mode_from_environment();
  bool watch = false;
  std::vector<std::string> argv;
  for (std::string_view a : args) {
    if (auto m = trace_mode_named(a)) trace_mode = *m;
    else if (a.starts_with("--trace-engine=")) {
      std::println(std::cerr, "ycxx: error: unknown trace mode in '{}' (stream, interactive or json)", a);
      return 1;
    } else if (a == "--watch") watch = true;
    else if (a == "--no-daemon") forced_off = true;
    else if (a == "--daemon") use = use == Use::require ? use : Use::on;
    else if (a == "--daemon=require") use = Use::require;
    else if (a == "-v") verbose = true;
    else expand(a, argv, 0);
  }
  if (forced_off) use = Use::off;

  compile::Request r{std::move(argv), cwd_now(), compile::environment_now(), now_seconds()};
  if (watch) {
    if (use == Use::off) {
      std::println(std::cerr, "ycxx: error: --watch needs the daemon, which watches the files (YCXX_NO_DAEMON or --no-daemon is set)");
      return 2;
    }
    if (trace_mode != TraceMode::off)
      r.trace = trace_mode == TraceMode::stream ? compile::Trace::events : compile::Trace::values;
    return watch_builds(std::move(r), verbose, trace_mode);
  }
  std::optional<TraceOut> traced;
  if (trace_mode != TraceMode::off) {
    traced.emplace(trace_mode);
    // The browser shows values; the stream does not, and skips rendering them.
    r.trace = trace_mode == TraceMode::stream ? compile::Trace::events : compile::Trace::values;
  }
  const auto finish = [&](compile::Done& d) {
    const int status = trace_mode == TraceMode::json ? finish_json(d) : compile::finish(d);
    if (traced) traced->finish();
    return status;
  };
  if (use != Use::off) {
    auto d = through_daemon(r, verbose, [&](daemon::TraceBatch&& b) {
      if (traced) traced->batch(std::move(b));
    }, [&] {
      if (traced) traced->restart("the daemon was lost; asking a new one");
    });
    if (d) return finish(*d);
    if (use == Use::require) {
      std::println(std::cerr, "ycxx: error: the daemon is required (YCXX_DAEMON=require) but failed: {}", d.error());
      return 2;
    }
    if (verbose) std::println(std::cerr, "ycxx: running in-process: {}", d.error());
    if (traced) traced->restart("running in-process");
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
int watch(daemon::Connection& c, bool interactive, bool json) {
  if (auto ok = daemon::watch_traces(c, interactive || json); !ok) {
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
                     },
                     std::format("daemon of {} (builds of files under it; --root picks another)", c.welcome.root)};
    trace::browse(tree, &feed);
    return 0;
  }
  const trace::Style style{!json && trace::colors_for(1), true, json};
  if (json) std::println(R"({{"watching":{},"pid":{}}})", trace::json_string(c.welcome.root), c.welcome.daemon_pid);
  else std::println("ycxx: watching the daemon of {} (pid {}): builds of files under it; ^C to stop", c.welcome.root, c.welcome.daemon_pid);
  std::cout.flush();  // a pipe buffers: whoever reads it knows the watch is attached
  std::map<std::uint64_t, trace::Stream> requests;
  std::uint64_t last = 0;
  while (auto b = daemon::next_trace(c)) {
    rebase(*b);
    auto [it, fresh] = requests.try_emplace(b->request, std::cout, style);
    if (fresh || last != b->request) {
      if (json) std::println(R"({{"request":{},"label":{}}})", b->request, trace::json_string(b->label));
      else it->second.note(std::format("── #{} {}", b->request, b->label));
    }
    last = b->request;
    if (b->end) {
      it->second.summary(b->ended_ns - std::min(b->ended_ns, b->started_ns));
      requests.erase(it);
      last = 0;
    } else {
      it->second.batch(b->events);
    }
  }
  // A watcher more than 64 MiB behind is dropped while the daemon lives on.
  const bool alive = static_cast<bool>(daemon::reach(c.welcome.root, false));
  if (json) std::println(R"({{"ended":{}}})", alive ? R"("dropped: this watch fell too far behind")" : R"("the daemon has gone")");
  else if (alive) std::println("ycxx: the daemon dropped this watch: it fell too far behind");
  else std::println("ycxx: the daemon has gone");
  return 0;
}

}  // namespace

// `ycxx ctl status|shutdown|trace [--root DIR]`: asks the daemon of DIR's scope (default: the
// working directory's), never starting one.
int control(std::span<const std::string_view> args) {
  std::optional<std::string> command, root;
  bool interactive = false, json = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--root" && i + 1 < args.size()) root = std::string(args[++i]);
    else if (args[i] == "--interactive") interactive = true;
    else if (args[i] == "--json") json = true;
    else if (!command) command = std::string(args[i]);
    else command.reset(), i = args.size();
  }
  if (!command || (*command != "status" && *command != "shutdown" && *command != "trace")) {
    std::println(std::cerr, "usage: ycxx ctl status|shutdown|trace [--root DIR] [--interactive|--json]");
    return 2;
  }
  std::string scope = root ? daemon::real_path(*root).value_or(*root) : daemon::root_for(std::nullopt, cwd_now());
  auto c = daemon::reach(scope, false);
  if (!c) {
    std::println(std::cerr, "ycxx: no daemon for {}: {}", scope, c.error());
    return 1;
  }
  if (*command == "trace") return watch(*c, interactive, json);
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
