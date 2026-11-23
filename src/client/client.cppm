// The daemon's clients (DECISIONS §4.7, daemon design §7.1): a c++-compatible command line, an
// LSP server and a control interface. `cxx` sends its request to the scope's daemon, starting it if
// needed, and runs the same request in-process on any failure; both paths run `compile::run`, so
// stdout, stderr, the exit status and the outputs are the same.
export module ycxx.client;

import std;
import ycxx.compile;
import ycxx.daemon;
import ycxx.engine;
import ycxx.memory;
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
      memory_ = b.memory;
      return;
    }
    trace::rebase(b.events, b.started_ns);
    events(b.events);
  }
  void total(std::uint64_t ns) { total_ = ns; }
  void memory(trace::BuildMemory m) { memory_ = m; }
  // A new run of the request (a retry, or the in-process fallback): its trace starts afresh, as
  // event numbers restart in another engine.
  void restart(std::string_view why) {
    if (mode_ == TraceMode::json) std::println(std::cerr, R"({{"restart":{}}})", trace::json_string(why));
    else std::println(std::cerr, "ycxx: {}: tracing it again", why);
    tree_ = {};
    stream_.reset();
    total_.reset();
    memory_.reset();
  }

  // After the compile: the stream's summary, or the browser (the stream, without a terminal).
  void finish() {
    if (mode_ == TraceMode::interactive) {
      if (memory_) tree_.set_memory(*memory_);
      if (trace::browse(tree_)) return;
      std::println(std::cerr, "ycxx: no terminal for --trace-engine=interactive; printing the trace");
      std::vector<trace::Event> all;
      for (const auto& n : tree_.nodes()) all.push_back(n.ev);
      stream_.batch(all);
    }
    stream_.summary(total_, memory_);
  }

 private:
  TraceMode mode_;
  trace::Stream stream_;
  trace::Tree tree_;
  std::optional<std::uint64_t> total_;
  std::optional<trace::BuildMemory> memory_;
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
// `said`, when given, takes the line `verbose` prints instead (a browser has the screen).
using OnTrace = std::function<void(daemon::TraceBatch&&)>;
std::expected<compile::Done, std::string> through_daemon(const compile::Request& r, bool verbose, const OnTrace& on_trace = {},
                                                         const std::function<void()>& on_retry = {}, std::uint64_t* instance = nullptr,
                                                         Interrupt* cut = nullptr, std::vector<std::string>* said = nullptr) {
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
      if (verbose) {
        std::string line = std::format("ycxx: answered by daemon {} for {} at revision {} ({} restarts, {} pp regions reused)",
                                       c->welcome.daemon_pid, c->welcome.root, d->report.revision, d->report.restarts, d->report.regions_reused);
        if (said) said->push_back(std::move(line));
        else std::println(std::cerr, "{}", line);
      }
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
  compile::write_outputs(d);
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
std::optional<std::uint64_t> await_change(const std::string& root, std::uint64_t since, std::uint64_t instance, Interrupt& cut,
                                          bool json = false, const std::function<void(std::vector<std::string>)>& on_watching = {}) {
  auto c = daemon::reach(root, true);
  if (!c) {
    const std::string why = "--watch: cannot reach the daemon: " + c.error();
    if (json) std::println(std::cerr, R"({{"error":{}}})", trace::json_string(why));
    else std::println(std::cerr, "ycxx: error: {}", why);
    return std::nullopt;
  }
  if (!cut.enter(c->fd.get())) return std::nullopt;
  auto changed = daemon::await_change(*c, since, instance, on_watching);
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
      changed = await_change(root, revision, instance, cut, json, [&](std::vector<std::string> paths) {
        if (json) {
          std::string list;
          for (const std::string& p : paths) list += (list.empty() ? "" : ",") + trace::json_string(p);
          std::println(std::cerr, R"({{"watching":[{}]}})", list);
          return;
        }
        // The project's files by name; the others (system headers) counted.
        std::vector<std::string> own;
        std::size_t others = 0;
        for (const std::string& p : paths) {
          if (p.starts_with(root + "/")) own.push_back(p.substr(root.size() + 1));
          else ++others;
        }
        std::string list;
        for (const std::string& p : own) list += (list.empty() ? "" : ", ") + p;
        std::println(std::cerr, "ycxx: watching {}{}{}; ^C to stop", list.empty() ? "no file of the project" : list,
                     others ? std::format(" and {} other file{} (system headers)", others, others == 1 ? "" : "s") : "",
                     "");
      });
      if (!changed) return 1;
    }
  }

  // The browser on this thread, the builds on another; what they find crosses in `items`.
  struct Item {
    enum { trace, done, files } what = trace;
    std::uint64_t build = 0;
    std::uint64_t space = 0;      // the build's event numbers, apart from every other run's
    std::uint64_t offset_ns = 0;  // the build's start, since the first's, on this process's clock
    daemon::TraceBatch batch;
    std::string label;
    std::string text;  // with `done`: the build's diagnostics and outputs' errors
    std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now();
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
        compile::write_outputs(*d);
        done.text = d->out + d->err;
        if (done.text.empty()) done.text = d->exit_status == 0 ? "built, no diagnostics" : "failed";
      } else {
        done.text = "the daemon could not build it: " + d.error();
      }
      post(std::move(done));
      changed = await_change(root, revision, instance, cut, false, [&](std::vector<std::string> paths) {
        std::string text = std::format("\n\nwatching {} files:\n", paths.size());
        for (const std::string& p : paths) text += "  " + p + "\n";
        post({Item::files, n, run, offset, {}, label, std::move(text)});
      });
      if (!changed) break;
    }
    std::lock_guard g(shared.m);
    shared.ended = true;
  });

  trace::Tree tree;
  const auto group_of = [](std::uint64_t build) { return (std::uint64_t{1} << 63) | build; };
  std::set<std::uint64_t> opened;
  // Where running cells' times reach (#939): the live batch's `sent_ns` on the tree's clock, when it
  // came on this one.
  std::uint64_t sent_ns = 0;
  auto sent_at = std::chrono::steady_clock::now();
  const auto open = [&](const Item& i, std::uint64_t start_ns) {
    if (opened.insert(i.build).second) tree.open_group(group_of(i.build), i.label, start_ns);
  };
  trace::Feed feed{.fd = wake->first.get(), .pump = [&] {
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
                       if (i.what == Item::files) {
                         tree.annotate(id, std::move(i.text), true);
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
                       if (b.end) {
                         tree.close_group(id, i.offset_ns + (b.ended_ns - std::min(b.ended_ns, start)));
                         tree.set_memory(b.memory);
                       } else if (b.live) {
                         tree.apply(b.events, id);
                         tree.set_memory(b.memory);
                         sent_ns = i.offset_ns + (b.sent_ns - std::min(b.sent_ns, start));
                         sent_at = i.received;
                       } else {
                         tree.add(b.events, id);
                       }
                     }
                     return !ended;
                   },
                   .source = std::format("rebuilding on every change of the files it reads ({})", root)};
  feed.tick = [&] {
    tree.advance(sent_ns + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - sent_at).count()));
    return true;
  };
  trace::browse(tree, &feed);
  // Stops the builder: a build in flight or its wait is cut, and the daemon cancels the build.
  cut.stop();
  return 0;
}

// `--trace-engine=interactive` on a terminal (#939): the browser opens at once and the build runs
// on a thread of its own, through the daemon (its live batches) or in-process (a `trace::Live`),
// so the tree fills in while it runs. The outputs are written and the diagnostics printed once the
// browser is closed; closing it before the build ends waits for the build.
int browse_compile(compile::Request r, Use use, bool verbose) {
  r.trace = compile::Trace::live;
  struct Shared {
    std::mutex m;
    std::vector<trace::Event> events;  // came and not yet in the tree
    std::uint64_t generation = 0;      // a new run of the request (a retry, the fallback): a new tree
    std::optional<trace::BuildMemory> memory;
    // The clock of running cells: the trace's `now_ns` at `at` on this process's clock.
    std::uint64_t now_ns = 0;
    std::chrono::steady_clock::time_point at{};
    trace::Live* live = nullptr;           // in-process: the engine's events are taken here
    const memory::Window* window = nullptr;  // in-process: the build's peak so far
    bool ended = false;
    std::expected<compile::Done, std::string> done = std::unexpected(std::string("not built"));
    int status = -1;  // when the build could not happen: what `cxx` returns
    std::vector<std::string> notes;  // for stderr, once the browser is closed
  } shared;
  std::vector<std::string> said;  // the builder's own: `-v`'s line
  const auto restart = [&](std::string why) {
    std::lock_guard g(shared.m);
    shared.events.clear();
    shared.memory.reset();
    shared.now_ns = 0;
    shared.at = std::chrono::steady_clock::now();
    ++shared.generation;
    shared.notes.push_back(std::format("ycxx: {}: traced again", why));
  };
  // The stack the daemon gives a request's thread: the compile's own recursion runs on it (#178).
  std::jthread builder(std::jthread::stack_size_hint(std::size_t{256} << 20), [&] {
    if (use != Use::off) {
      auto d = through_daemon(r, verbose, [&](daemon::TraceBatch&& b) {
        std::lock_guard g(shared.m);
        if (b.end) {
          shared.memory = b.memory;
          return;
        }
        trace::rebase(b.events, b.started_ns);
        shared.events.append_range(std::move(b.events));
        shared.memory = b.memory;
        shared.now_ns = b.sent_ns - std::min(b.sent_ns, b.started_ns);
        shared.at = std::chrono::steady_clock::now();
      }, [&] { restart("the daemon was lost; asking a new one"); }, nullptr, nullptr, &said);
      if (d || use == Use::require) {
        std::lock_guard g(shared.m);
        if (!d) {
          shared.notes.push_back(std::format("ycxx: error: the daemon is required (YCXX_DAEMON=require) but failed: {}", d.error()));
          shared.status = 2;
        }
        shared.done = std::move(d);
        shared.ended = true;
        return;
      }
      if (verbose) restart(std::format("running in-process: {}", d.error()));
      else restart("running in-process");
    }
    pp::DiskHost disk;
    const compile::Threads t = compile::threads();
    engine::Pool pool(t.count, t.chaos_seed);
    pipeline::Session session(pool);
    bool stopped = false;
    const memory::Window window;  // the process's peak while it builds (#274)
    trace::Live live(true);
    {
      std::lock_guard g(shared.m);
      shared.live = &live;
      shared.window = &window;
    }
    compile::Done d = compile::run(r, disk, session, {}, stopped, {}, &live);
    std::lock_guard g(shared.m);
    shared.events.append_range(live.take());
    shared.memory = trace::BuildMemory{memory::process().current, window.peak()};
    shared.live = nullptr;
    shared.window = nullptr;
    shared.done = std::move(d);
    shared.ended = true;
  });

  trace::Tree tree;
  std::uint64_t generation = 0;
  trace::Feed feed;
  feed.source = std::format("building {}", compile::first_input(r.argv).value_or("…"));
  feed.ended = "the build has ended";
  feed.newest_first = false;  // one request: its cells in the order they started, as after a build
  feed.tick = [&] {
    std::vector<trace::Event> events;
    std::optional<trace::BuildMemory> mem;
    std::uint64_t now = 0;
    bool ended = false;
    {
      std::lock_guard g(shared.m);
      if (shared.generation != generation) {
        generation = shared.generation;
        tree.reset();  // the browser sees it, and lets go of what it held
      }
      events.swap(shared.events);
      if (shared.live) {
        events.append_range(shared.live->take());
        now = shared.live->now_ns();
        shared.memory = trace::BuildMemory{memory::process().current, shared.window->peak()};
      } else {
        now = shared.now_ns + static_cast<std::uint64_t>(
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - shared.at).count());
      }
      mem = shared.memory;
      ended = shared.ended;
    }
    tree.apply(events);
    if (mem) tree.set_memory(*mem);
    if (ended) {
      tree.settle();
      return false;
    }
    tree.advance(now);
    return true;
  };
  const bool browsed = trace::browse(tree, &feed);
  bool ended = false;
  {
    std::lock_guard g(shared.m);
    ended = shared.ended;
  }
  if (browsed && !ended) std::println(std::cerr, "ycxx: the trace browser closed; waiting for the build to end");
  builder.join();
  for (const std::string& n : shared.notes) std::println(std::cerr, "{}", n);
  for (const std::string& n : said) std::println(std::cerr, "{}", n);
  if (!browsed) {
    // The terminal went away after all: the trace as the stream, after the build, as without one.
    std::println(std::cerr, "ycxx: no terminal for --trace-engine=interactive; printing the trace");
    while (feed.tick()) {
    }
    trace::Stream stream(std::cerr, {trace::colors_for(2), true, false});
    std::vector<trace::Event> all;
    for (std::int32_t i : tree.preorder()) all.push_back(tree.node(i).ev);
    stream.batch(all);
    stream.summary(std::nullopt, tree.memory());
  }
  if (shared.status >= 0) return shared.status;
  if (!shared.done) {
    std::println(std::cerr, "ycxx: error: {}", shared.done.error());
    return 1;
  }
  return compile::finish(*shared.done);
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
  // Counting memory serves a trace (#274): without one nothing reads the counts, and every
  // allocation from here on pays only a load and a branch.
  if (trace_mode == TraceMode::off) memory::disable();

  if (auto query = compile::driver_query(argv)) return compile::finish(*query);

  compile::Request r{std::move(argv), cwd_now(), compile::environment_now(), now_seconds()};
  if (watch) {
    if (use == Use::off) {
      std::println(std::cerr, "ycxx: error: --watch needs the daemon, which watches the files (YCXX_NO_DAEMON or --no-daemon is set)");
      return 2;
    }
    if (trace_mode != TraceMode::off)
      r.trace = trace_mode == TraceMode::stream ? compile::Trace::events : compile::Trace::values;
    // The browser shows each build while it runs (#939).
    if (trace_mode == TraceMode::interactive && trace::is_terminal(0)) r.trace = compile::Trace::live;
    return watch_builds(std::move(r), verbose, trace_mode);
  }
  // The browser while the build runs (#939), when there is a terminal for it; without one, the
  // trace is gathered and printed as the stream after the build, as before.
  if (trace_mode == TraceMode::interactive && trace::has_terminal()) return browse_compile(std::move(r), use, verbose);
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
      const std::string why = "the daemon is required (YCXX_DAEMON=require) but failed: " + d.error();
      if (trace_mode == TraceMode::json) std::println(std::cerr, R"({{"error":{}}})", trace::json_string(why));
      else std::println(std::cerr, "ycxx: error: {}", why);
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
  std::optional<memory::Window> window;  // the process's peak while it builds (#274)
  if (traced) window.emplace();
  const auto start = std::chrono::steady_clock::now();
  if (traced) collector.emplace([&](std::span<const trace::Event> batch) { traced->events(batch); }, r.trace == compile::Trace::values, start);
  compile::Done d = compile::run(r, disk, session, {}, stopped, {}, collector ? &*collector : nullptr);
  if (collector) {
    collector->finish();
    traced->total(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
    traced->memory({memory::process().current, window->peak()});
  }
  return finish(d);
}

// `ycxx lsp [--root DIR]`: the editor's language server, which runs in the daemon of DIR's scope
// (default: the working directory's), started if none runs; this process only copies bytes between
// its stdio and the daemon (daemon design §6.4).
int lsp(std::span<const std::string_view> args) {
  memory::disable();  // this process only copies bytes; the daemon counts (#274)
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
  // The browser gets live batches: each request fills in while it runs (#939).
  const bool browsing = interactive && trace::is_terminal(0);
  if (auto ok = daemon::watch_traces(c, interactive || json, browsing); !ok) {
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
  if (browsing) {
    trace::Tree tree;
    std::set<std::uint64_t> seen;
    std::uint64_t sent_ns = 0;  // where running cells' times reach (#939), when it came
    auto sent_at = std::chrono::steady_clock::now();
    trace::Feed feed{.fd = c.fd.get(), .pump = [&] {
                       auto b = daemon::next_trace(c);
                       if (!b) {
                         tree.settle();  // what was running when the daemon went never ends
                         return false;
                       }
                       const std::uint64_t sent = b->sent_ns;
                       rebase(*b);
                       if (seen.insert(b->request).second) tree.open_group(group_of(b->request), std::format("#{} {}", b->request, b->label), b->started_ns);
                       if (b->end) {
                         tree.close_group(group_of(b->request), b->ended_ns);
                         tree.set_memory(b->memory);
                       } else if (b->live) {
                         tree.apply(b->events, group_of(b->request));
                         tree.set_memory(b->memory);
                         sent_ns = sent - std::min(sent, *base);
                         sent_at = std::chrono::steady_clock::now();
                       } else {
                         tree.add(b->events, group_of(b->request));
                       }
                       return true;
                     },
                     .source = std::format("daemon of {} (builds of files under it; --root picks another)", c.welcome.root)};
    feed.tick = [&] {
      tree.advance(sent_ns + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - sent_at).count()));
      return true;
    };
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
      it->second.summary(b->ended_ns - std::min(b->ended_ns, b->started_ns), b->memory);
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
  memory::disable();  // the daemon's numbers are the daemon's (#274)
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
  // What each kind retains (#274), one line each: `retained_by_kind: Tu 1234567 bytes in 12 cells`.
  template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^daemon::Status,
                                                                                                  std::meta::access_context::unchecked()))) {
    if constexpr (std::formattable<typename[:std::meta::type_of(m):], char>) {
      std::println("{}: {}", std::meta::identifier_of(m), s->[:m:]);
    } else {
      for (const daemon::KindRetained& k : s->retained_by_kind)
        std::println("{}: {} {} bytes in {} cells", std::meta::identifier_of(m), k.kind, k.retained, k.values);
    }
  }
  return 0;
}

}  // namespace ycxx::client
