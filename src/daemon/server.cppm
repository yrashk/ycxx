// The daemon process (daemon design §3, §4, §5, §7): one per scope root and ycxx binary, holding
// the scope's lifetime lock, listening on the scope's socket, and answering `compile`, `status` and
// `shutdown` requests, one connection per thread.
//
// Requests are admitted one at a time into the engine: the pipeline still keeps process-wide state
// (pp's session, #63), so an epoch is one request, and the epoch strand that runs many requests at
// once is #15's. Between epochs the watcher's dirty paths are applied to the file table, after a
// cookie shows every change made before the request was sent has been queued (§5.4). A batch that
// reaches the table while a request runs stops it before its next translation unit and runs it
// again at the new revision, behind the progress quantum (§4.1, §4.2), so the client sees one
// answer. A client that hangs up stops its request (§3.2).
//
// An `lsp` connection is an LSP session (§6.4): its documents' buffers are the engine's editor
// view, and each diagnosis is one more request admitted the same way, checking the document's
// translation unit with the command line a served `compile` gave it.
//
// A `watch` connection (#245) receives the engine trace of every request served from then on;
// a `compile` that asks for its trace receives its own, before its answer.
export module ycxx.daemon:server;

import std;
import ycxx.build.plan;
import ycxx.build.project;
import ycxx.compile;
import ycxx.engine;
import ycxx.lsp;
import ycxx.memory;
import ycxx.pipeline;
import ycxx.pp;
import ycxx.trace;
import :posix;
import :os;
import :scope;
import :protocol;
import :events;
import :files;
import :watch;
import :admission;

export namespace ycxx::daemon {

struct ServerOptions {
  std::string root;
  std::optional<int> ready_fd;  // written one byte once listening (§7.1)
  std::chrono::seconds idle_exit{30 * 60};
  std::chrono::milliseconds quantum_floor{50};
  std::chrono::milliseconds cookie_timeout{2000};
  bool watch = true;  // false under YCXX_NO_WATCH=1: every request rescans the table (§5.4)
  pipeline::BuildCheckpoint build_checkpoint;  // process-test driver only
};

// Runs the daemon until idle exit, `shutdown` or SIGTERM; the process's exit status.
int serve(const ServerOptions& o);

}  // namespace ycxx::daemon

namespace ycxx::daemon {
namespace {

using Clock = std::chrono::steady_clock;

// A request's thread stack (#178): fixed, so the daemon does not depend on the stack limit of the
// client that started it, and far above a main thread's default (8 MiB), so a request that fits
// in-process fits here. Only address space is reserved. Deeper recursion needs a guard (#187).
constexpr std::size_t request_stack = std::size_t{256} << 20;

void log(std::string_view what) {
  auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
  std::println(std::cerr, "[{}] ycxx daemon {}: {}", now.count(), my_pid(), what);
}

class Server {
 public:
  explicit Server(const ServerOptions& o, Paths paths)
      : o_(o),
        paths_(std::move(paths)),
        env_(compile::environment_now()),
        table_([this](const std::string& dir) {
          if (watcher_) watcher_->add(dir);
        }),
        admission_(o.quantum_floor) {}

  int run() {
    ignore_sigpipe();
    auto lock = lock_file(paths_.lock(), std::chrono::seconds(10));
    if (!lock) {
      log(lock.error());
      return 1;
    }
    lock_ = std::move(*lock);
    // The lock is ours: a socket file left by a daemon that died is stale.
    remove_file(paths_.socket());
    auto listener = listen_unix(paths_.socket());
    if (!listener) {
      log(listener.error());
      return 1;
    }
    listener_ = std::move(*listener);
    auto term = termination_pipe();
    if (!term) {
      log(term.error());
      return 1;
    }
    auto wake = make_pipe();
    if (!wake) {
      log(wake.error());
      return 1;
    }
    wake_r_ = std::move(wake->first);
    wake_w_ = std::move(wake->second);
    // A relative SDK override is resolved against each request's directory and watched when read.
    const char* sdkroot = std::getenv("SDKROOT");
    for (const std::string& d : pp::all_system_dirs(sdkroot ? sdkroot : ""))
      system_dirs_.push_back(std::filesystem::path(d).lexically_normal().generic_native_encoded_string());
    start_watching();
    log(std::format("serving {} on {} (watcher: {})", paths_.root, paths_.socket(), watcher_state_));
    if (o_.ready_fd) {
      char c = 'r';
      write_all(*o_.ready_fd, std::as_bytes(std::span(&c, 1)));
      Fd(*o_.ready_fd).reset();
    }

    last_activity_ = Clock::now();
    const int fds[3] = {listener_.get(), term->get(), wake_r_.get()};
    bool stopping = false;
    while (!stopping) {
      auto ready = wait_any(fds, std::chrono::seconds(1));
      for (std::size_t i : ready) {
        if (i == 0) {
          if (auto c = accept_one(listener_.get())) start_connection(std::move(*c));
        } else if (i == 1) {
          log("terminated by a signal");
          stopping = true;
        } else {
          drain(wake_r_.get());
        }
      }
      if (shutdown_requested_) stopping = true;
      std::lock_guard g(conn_mu_);
      if (connections_ == 0 && Clock::now() - last_activity_ >= o_.idle_exit) {
        log("idle: exiting");
        stopping = true;
      }
    }
    // Finish the requests in flight (§7.4), then remove the socket while still holding the lock.
    // An LSP session lasts as long as its editor: it is ended, and its `ycxx lsp` exits.
    listener_.reset();
    remove_file(paths_.socket());
    std::unique_lock g(conn_mu_);
    lsp_closed_ = true;
    for (int fd : lsp_fds_) shutdown_both(fd);
    {
      std::lock_guard w(watch_mu_);
      for (const auto& watcher : watchers_) shutdown_both(watcher->fd);
    }
    conn_cv_.wait(g, [&] { return connections_ == 0; });
    log("exited");
    return 0;
  }

 private:
  void wake() {
    char c = 'w';
    write_all(wake_w_.get(), std::as_bytes(std::span(&c, 1)));
  }

  // The cookie directory is the runtime directory when it is on the root's volume; otherwise
  // `<root>/.ycxx-cookies`, ignored by version control (§5.4). It must be a directory of this user,
  // closed to others and not a symbolic link, so a repository cannot point the daemon's writes
  // elsewhere; if it cannot be, the runtime directory serves.
  std::string cookie_dir() {
    auto run_dir = real_path(paths_.dir).value_or(paths_.dir);
    auto a = stat_path(run_dir);
    auto b = stat_path(paths_.root);
    if (!a || !b || a->dev == b->dev) return run_dir;
    std::string dir = paths_.root + "/.ycxx-cookies";
    if (auto ok = ensure_private_dir(dir); !ok) {
      log(ok.error() + "; cookies go to the runtime directory");
      return run_dir;
    }
    create_new_file(dir + "/.gitignore", "*\n");  // an existing one is kept
    return real_path(dir).value_or(dir);
  }

  // A cookie directory removed under the daemon (`git clean -fdx`, a temporary-file cleaner) is
  // created and watched again, so freshness does not degrade to a rescan per request.
  void renew_cookie_dir() {
    const std::string& dir = events_->cookie_dir();
    if (stat_path(dir)) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ensure_private_dir(dir);
    if (dir.ends_with("/.ycxx-cookies")) create_new_file(dir + "/.gitignore", "*\n");
    if (watcher_) watcher_->renew(dir);
  }

  void start_watching() {
    events_.emplace(cookie_dir());
    if (!o_.watch) {
      watcher_state_ = "none (YCXX_NO_WATCH=1)";
      return;
    }
    watcher_.emplace(*events_);
    std::vector<std::string> dirs{events_->cookie_dir()};
    if (os_name == "macos") {
      // One recursive stream: the root, the toolchain and the sysroot from the start (§5.5).
      dirs.push_back(paths_.root);
      for (const std::string& d : system_dirs_)
        if (auto r = real_path(d)) dirs.push_back(*r);
    }
    if (auto err = watcher_->start(std::move(dirs), paths_.root)) {
      watcher_state_ = std::format("none ({})", *err);
      watcher_.reset();
      return;
    }
    watcher_state_ = std::string(watcher_->name());
  }

  void start_connection(Fd c) {
    {
      std::lock_guard g(conn_mu_);
      ++connections_;
    }
    // A request recurses as deep in the daemon as in-process, on a main thread's stack (#178).
    std::thread(std::thread::stack_size_hint(request_stack), [this, c = std::move(c)]() mutable {
      serve_connection(c.get());
      c.reset();
      std::lock_guard g(conn_mu_);
      --connections_;
      last_activity_ = Clock::now();
      conn_cv_.notify_all();
    }).detach();
  }

  void serve_connection(int fd) {
    // Only this user's processes may connect (§3.1).
    if (peer_uid(fd) != my_uid()) return;
    if (!wait_readable(fd, std::chrono::seconds(5))) return;
    auto hello_frame = receive(fd);
    if (!hello_frame || hello_frame->kind != Kind::hello) return;
    auto hello = decode<Hello>(*hello_frame);
    if (!hello) return;
    if (hello->build_hash != build_hash() || hello->schema_hash != schema_hash || hello->uid != my_uid()) {
      send(fd, Kind::refused, Refused{"a different ycxx build"});
      return;
    }
    if (!send(fd, Kind::welcome, Welcome{my_pid(), paths_.root, instance_})) return;
    auto frame = receive(fd);
    if (!frame) return;
    switch (frame->kind) {
      case Kind::status:
        send(fd, Kind::status_reply, status());
        return;
      case Kind::shutdown:
        log("shutdown requested");
        shutdown_requested_ = true;
        wake();
        send(fd, Kind::bye, Bye{});
        return;
      case Kind::lsp:
        serve_lsp(fd);
        return;
      case Kind::watch:
        if (auto r = decode<WatchRequest>(*frame)) serve_watch(fd, *r);
        return;
      case Kind::await_change:
        if (auto r = decode<AwaitChange>(*frame)) serve_await_change(fd, *r);
        return;
      case Kind::project_build:
        if (auto r = decode<ProjectBuildRequest>(*frame)) serve_project_build(fd, *r);
        return;
      case Kind::compile:
        if (auto r = decode<compile::Request>(*frame)) serve_compile(fd, *r);
        return;
      default:
        return;  // a malformed session closes the connection (§3.1)
    }
  }

  Status status() {
    std::lock_guard g(stats_mu_);
    Status s = stats_;
    s.pid = my_pid();
    s.root = paths_.root;
    s.socket = paths_.socket();
    s.watcher = watcher_state_;
    s.idle_exit_s = static_cast<std::uint64_t>(o_.idle_exit.count());
    s.threads = threads_.count;
    s.peak_running = session().peak_requests();  // through the once: `session_` may be being made
    const memory::Totals m = memory::process();
    s.memory_current = m.current;
    s.memory_peak = m.peak;
    s.memory_allocated = m.allocated;
    s.allocations = m.count;
    for (const engine::KindMemory& k : session().db().memory_per_kind()) {
      s.memory_retained += k.retained;
      if (k.values > 0) s.retained_by_kind.push_back({trace::short_kind(k.kind), k.retained, k.values});
    }
    s.memory_unattributed = m.current - s.memory_retained;
    std::ranges::stable_sort(s.retained_by_kind, std::greater{}, &KindRetained::retained);
    return s;
  }

  // Any readable byte after the request, or end of file, is a hang-up: the client sends nothing
  // more (§3.2).
  static bool hung_up(int fd) { return wait_readable(fd, std::chrono::milliseconds(0)); }

  // Applies what the watcher queued: the batch between two epochs (§5.3). Without a watcher, or
  // after an overflow, the whole table is rescanned (§5.4).
  std::size_t apply_batch(bool rescan) {
    Events::Batch b = events_->drain();
    std::size_t changed = 0;
    if (rescan || b.overflow || !watcher_) {
      changed = table_.rescan();
      std::lock_guard g(stats_mu_);
      ++stats_.rescans;
    } else {
      changed = table_.apply(b.paths);
      // Directories past the watch limit are stat-checked instead (§5.5).
      if (auto un = watcher_->unwatched(); !un.empty()) changed += table_.rescan(std::span<const std::string>(un));
    }
    set_changed_inputs();
    std::lock_guard g(stats_mu_);
    if (changed > 0) {
      ++stats_.revision;
      ++stats_.batches;
      stats_.inputs_changed += changed;
    }
    if (watcher_) stats_.unwatched = watcher_->unwatched().size();
    return changed;
  }

  // ---- Traces (#245)

  // Where a request's trace frames go: an attached `ycxx ctl trace`, or the client of a traced
  // `compile`. Requests queue frames; the connection's own thread writes them, so a reader that
  // stops reading never blocks a pool worker. A watcher more than `max_queued` bytes behind is
  // dropped (a frame is always taken into an empty queue, whatever its size).
  struct Watching {
    static constexpr std::size_t max_queued = std::size_t{64} << 20;
    int fd = -1;
    bool values = false;
    bool live = false;  // live batches (#939)
    std::size_t cap = max_queued;  // the requesting client's has none: its own thread drains it
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<std::byte>> queue;  // under m
    std::size_t queued = 0;                    // bytes, under m
    bool gone = false;                         // under m
    bool dropped = false;                      // gone for falling behind; under m
    void push(std::vector<std::byte> frame) {
      std::lock_guard g(m);
      if (gone) return;
      if (!queue.empty() && queued + frame.size() > cap) {
        gone = dropped = true;
        cv.notify_all();
        return;
      }
      queued += frame.size();
      queue.push_back(std::move(frame));
      cv.notify_all();
    }
    // Writes what is queued, on the calling thread; false once it is gone.
    bool flush() {
      std::unique_lock g(m);
      if (gone) return false;
      std::deque<std::vector<std::byte>> frames;
      frames.swap(queue);
      queued = 0;
      g.unlock();
      bool ok = true;
      for (const auto& f : frames) ok = ok && write_all(fd, f);
      g.lock();
      if (!ok) gone = true;
      return ok;
    }
  };

  // One request's trace: a collector whose batches go to the request's client, when it asked,
  // and to each watcher attached when it started. Those that asked for a live trace (#939) get
  // instead what came since the last batch, every frame's start included, on each of the
  // request's ticks (`pump`). `finish` adds the `end` batch; the client's frames are written by
  // its connection thread (`flush_client`), before the answer.
  class RequestTrace {
   public:
    RequestTrace(Server& s, int client, compile::Trace asked, std::string label, std::vector<std::shared_ptr<Watching>> watchers)
        : server_(&s) {
      bool values = asked == compile::Trace::values || asked == compile::Trace::live;
      if (client >= 0) {
        auto c = std::make_shared<Watching>();
        c->fd = client;
        c->cap = std::numeric_limits<std::size_t>::max();  // bounded by the request itself
        c->live = asked == compile::Trace::live;
        client_ = c;
        (c->live ? live_to_ : whole_to_).push_back(c);
      }
      for (auto& w : watchers) {
        values = values || w->values;
        (w->live ? live_to_ : whole_to_).push_back(std::move(w));
      }
      batch_.request = s.trace_requests_.fetch_add(1, std::memory_order_relaxed) + 1;
      batch_.label = std::move(label);
      batch_.started_ns = s.trace_clock();
      live_batch_.request = batch_.request;
      live_batch_.label = batch_.label;
      live_batch_.started_ns = batch_.started_ns;
      live_batch_.live = true;
      const auto origin = s.trace_origin_;
      if (!whole_to_.empty()) collector_.emplace([this](std::span<const trace::Event> events) { deliver(events); }, values, origin);
      if (!live_to_.empty()) live_.emplace(values, origin);
      if (collector_ && live_) tee_.emplace(*collector_, *live_);
    }
    RequestTrace(const RequestTrace&) = delete;
    engine::Tracer* tracer() {
      if (tee_) return &*tee_;
      if (live_) return &*live_;
      return &*collector_;
    }

    // On the request's thread, as it runs: what came since the last live batch, to the live.
    void pump() {
      if (!live_) return;
      std::vector<trace::Event> events = live_->take();
      if (events.empty() && live_sent_) return;  // the first batch goes even empty: the request has started
      live_sent_ = true;
      live_batch_.events = std::move(events);
      live_batch_.sent_ns = server_->trace_clock();
      live_batch_.memory = {memory::process().current, window_.peak()};
      engine::Sink body;
      engine::serialize(body, live_batch_);
      const std::vector<std::byte> frame = encode_frame(Kind::trace, body.bytes());
      for (const auto& w : live_to_) w->push(frame);
    }

    // Once the request's frames have all finished (it completed, or `Session::run` waited for
    // its cancelled runs): orphans, then the `end` batch.
    void finish() {
      if (collector_) collector_->finish();
      pump();
      batch_.end = true;
      batch_.ended_ns = server_->trace_clock();
      batch_.memory = {memory::process().current, window_.peak()};
      batch_.events.clear();
      engine::Sink body;
      engine::serialize(body, batch_);
      const std::vector<std::byte> frame = encode_frame(Kind::trace, body.bytes());
      for (const auto& w : whole_to_) w->push(frame);
      for (const auto& w : live_to_) w->push(frame);
    }
    // On the client's connection thread.
    void flush_client() {
      if (client_) client_->flush();
    }

   private:
    // Under the collector's lock.
    void deliver(std::span<const trace::Event> events) {
      batch_.events.assign(events.begin(), events.end());
      engine::Sink body;
      engine::serialize(body, batch_);
      const std::vector<std::byte> frame = encode_frame(Kind::trace, body.bytes());
      for (const auto& w : whole_to_) w->push(frame);
    }

    Server* server_;
    std::shared_ptr<Watching> client_;
    std::vector<std::shared_ptr<Watching>> whole_to_, live_to_;  // the client among them, when it asked
    TraceBatch batch_, live_batch_;
    std::optional<trace::Collector> collector_;
    std::optional<trace::Live> live_;
    std::optional<trace::Tee> tee_;
    bool live_sent_ = false;
    memory::Window window_;  // the process's peak while the request runs (#274)
  };

  // A request's trace when its client asked for one or someone watches; nothing otherwise, so an
  // untraced request costs nothing. Watchers that are gone are skipped, not removed: their own
  // threads remove them, and the stop must still find them to shut them down.
  std::unique_ptr<RequestTrace> trace_for(int client, compile::Trace asked, std::string label) {
    std::vector<std::shared_ptr<Watching>> watchers;
    {
      std::lock_guard g(watch_mu_);
      for (const auto& w : watchers_) {
        std::lock_guard l(w->m);
        if (!w->gone) watchers.push_back(w);
      }
    }
    if (asked == compile::Trace::off && watchers.empty()) return nullptr;
    return std::make_unique<RequestTrace>(*this, asked == compile::Trace::off ? -1 : client, asked, std::move(label), std::move(watchers));
  }
  std::uint64_t trace_clock() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - trace_origin_).count());
  }
  static std::string label_of(std::string_view front, const compile::Request& r) {
    std::string out(front);
    for (const std::string& a : r.argv) out += " " + a;
    return out;
  }

  // Attaches a watcher until it hangs up or the daemon stops (which shuts its socket).
  void serve_watch(int fd, const WatchRequest& r) {
    auto w = std::make_shared<Watching>();
    w->fd = fd;
    w->values = r.values;
    w->live = r.live;
    {
      // Registered under the stop's own lock, so the stop either refuses it or shuts it down.
      std::lock_guard g(conn_mu_);
      if (lsp_closed_) return;
      std::lock_guard l(watch_mu_);
      watchers_.push_back(w);
    }
    log("a trace watcher attached");
    // The ack first, then what requests queue, until the watcher hangs up (it sends nothing, so a
    // readable socket is its hang-up, or the stop's shutdown), a write fails, or it falls behind.
    bool ok = send(fd, Kind::watch, WatchAck{});
    while (ok) {
      {
        std::unique_lock g(w->m);
        w->cv.wait_for(g, std::chrono::milliseconds(250), [&] { return w->gone || !w->queue.empty(); });
      }
      ok = !hung_up(fd) && w->flush();
    }
    bool dropped = false;
    {
      std::lock_guard g(w->m);
      w->gone = true;
      dropped = w->dropped;
    }
    {
      std::lock_guard l(watch_mu_);
      std::erase(watchers_, w);
    }
    log(dropped ? "a trace watcher fell behind and was dropped" : "a trace watcher detached");
  }

  // Waits between watch builds (#245, #1263). An observed plan change wakes a project build even
  // if no compiler file input changed. Otherwise the revision must pass the one the last build
  // started from, whoever applied the batch (another request, the LSP, or this wait, which
  // applies batches as a request's tick would). Ends unanswered on a hang-up or the stop, which
  // shuts it down like an LSP session.
  void serve_await_change(int fd, const AwaitChange& r) {
    const auto revision = [&] {
      std::lock_guard g(stats_mu_);
      return stats_.revision;
    };
    if (r.instance != 0 && r.instance != instance_) {
      send(fd, Kind::changed, Changed{0});  // another daemon's build: this one has not seen these files
      return;
    }
    {
      std::lock_guard g(conn_mu_);
      if (lsp_closed_) return;
      lsp_fds_.insert(fd);
    }
    // No build answered (refused, failed): wait from now, never answer at once.
    const std::uint64_t since = r.instance == 0 ? revision() : r.since;
    std::vector<std::string> paths = table_.present_keys();
    const auto observe_project = [&] {
      (void)session();
      return build_->watch_state(r.project->cwd, r.project->target, r.project->tool_path);
    };
    bool plan_changed = false;
    if (r.project) {
      auto observed = observe_project();
      paths.append_range(std::move(observed.paths));
      std::ranges::sort(paths);
      paths.erase(std::ranges::unique(paths).begin(), paths.end());
      plan_changed = observed.stamp != r.project->stamp;
    }
    send(fd, Kind::watching, WatchedFiles{std::move(paths)});
    // Without a watcher a batch is a rescan of the whole table: less often.
    const auto every = watcher_ ? std::chrono::milliseconds(150) : std::chrono::milliseconds(1000);
    do {
      if (const std::uint64_t now = revision(); now > since || plan_changed) {
        send(fd, Kind::changed, Changed{now > since ? now - since : 1});
        break;
      }
      {
        std::lock_guard g(conn_mu_);
        if (lsp_closed_) break;
      }
      {
        std::unique_lock strand(strand_mu_, std::try_to_lock);
        if (strand && (running_ == 0 || admission_.may_apply(Clock::now())) && apply_batch(false) > 0)
          admission_.applied(Clock::now());
      }
      if (wait_readable(fd, every)) break;
      if (r.project) plan_changed = observe_project().stamp != r.project->stamp;
    } while (true);
    std::lock_guard g(conn_mu_);
    lsp_fds_.erase(fd);
  }

  void serve_project_build(int fd, const ProjectBuildRequest& r) {
    auto cwd = real_path(r.cwd);
    if (!cwd || r.target.empty() || r.target == "." || r.target == ".." ||
        r.target.contains('/') || r.target.contains('\0')) {
      send(fd, Kind::project_build_done, ProjectBuildDone{1, {}, {"invalid workspace or target name"}, 0});
      return;
    }
    auto& sess = session();
    auto mode = compile::Trace::off;
    if (r.trace) mode = r.trace_live ? compile::Trace::live : r.trace_values ? compile::Trace::values : compile::Trace::events;
    auto traced = trace_for(fd, mode, "build " + r.target);
    std::optional<WatchedProject> watched;
    if (r.watch) {
      const auto observed = build_->watch_state(*cwd, r.target, r.tool_path);
      watched = WatchedProject{*cwd, r.target, observed.stamp, r.tool_path};
    }
    std::optional<std::uint64_t> started_at;
    auto refresh = [&] {
      std::lock_guard strand(strand_mu_);
      catch_up();
      if (!started_at && watched) {
        std::lock_guard g(stats_mu_);
        started_at = stats_.revision;
      }
    };
    auto cancelled = [&] { return hung_up(fd); };
    auto apply = [&](const std::function<void(engine::Db&)>& write) {
      std::lock_guard strand(strand_mu_);
      restarted(sess.apply(write));
    };
    auto publish = [&](engine::Revision revision, const std::function<void()>& write) {
      std::lock_guard strand(strand_mu_);
      if (cancelled()) return false;
      catch_up();
      if (sess.db().revision() != revision || cancelled()) return false;
      write();
      return true;
    };
    auto tick = [&] {
      if (traced && !cancelled()) traced->pump(), traced->flush_client();
    };
    auto report = build_->run(*cwd, r.target, r.tool_path, r.watch, r.probe_toolchain,
                              refresh, cancelled, apply, publish, tick, traced ? traced->tracer() : nullptr);
    if (watched && report.watch_stamp) watched->stamp = *report.watch_stamp;
    if (traced) {
      traced->finish();
      if (!cancelled()) traced->flush_client();
    }
    if (!cancelled()) send(fd, Kind::project_build_done, ProjectBuildDone{
        report.status, std::move(report.output), std::move(report.diagnostics), report.actions_executed,
        started_at.value_or(0), std::move(watched)});
  }

  void serve_compile(int fd, const compile::Request& r) {
    if (r.env != env_) {
      send(fd, Kind::refused, Refused{"the environment differs from the daemon's"});
      return;
    }
    {
      std::lock_guard g(stats_mu_);
      ++stats_.live;
    }
    auto traced = trace_for(fd, r.trace, label_of("cxx", r));
    auto done = run_compile(fd, r, traced.get());
    if (traced) {
      traced->finish();
      if (done) traced->flush_client();  // its last batches before the answer
      traced.reset();
    }
    {
      std::lock_guard g(stats_mu_);
      --stats_.live;
      if (done) {
        ++stats_.requests;
        stats_.restarts += done->report.restarts;
        stats_.regions_computed += done->report.regions_computed;
        stats_.regions_reused += done->report.regions_reused;
        stats_.executed += done->report.executed;
        stats_.validated += done->report.validated;
        stats_.last_executed = done->report.executed;
      } else {
        ++stats_.cancelled;
      }
    }
    if (done) {
      remember(r);
      send(fd, Kind::done, *done);
    }
  }

  // A file's real path (its directory's, for a file not on disk yet): the name a build's cells and
  // the file table know it by, whatever symbolic link the editor reached it through.
  static std::string canonical(const std::string& path) {
    if (auto r = real_path(path)) return *r;
    const std::filesystem::path p(path);
    if (auto dir = real_path(p.parent_path().native_encoded_string()))
      return (std::filesystem::path(*dir) / p.filename()).native_encoded_string();
    return path;
  }

  // The command line of each source a served request compiled, by its real path: an editor's
  // document is checked with it (§6.4; `compile_commands.json` is the fallback, `listed_request`).
  // One copy of the request serves all its sources.
  void remember(const compile::Request& r) {
    auto shared = std::make_shared<const compile::Request>(r);
    std::vector<std::pair<std::string, std::string>> sources;
    for (std::string& s : compile::sources_of(r)) sources.emplace_back(canonical(s), std::move(s));
    std::lock_guard g(commands_mu_);
    for (auto& [real, spelled] : sources) commands_.insert_or_assign(std::move(real), Command{shared, std::move(spelled)});
  }

  // `compile_commands.json` at the scope's root (§6.4): a file's command line there, by its real
  // path, checked with that line's preprocessing options; nothing for a file that is no
  // translation unit ycxx compiles (a header, a C file). The file is read again when it changes
  // (its identity: size and times).
  std::optional<compile::Request> listed_request(const std::string& real) {
    const std::string path = (std::filesystem::path(paths_.root) / "compile_commands.json").native_encoded_string();
    std::lock_guard g(listed_mu_);
    if (const std::optional<Identity> id = stat_path(path); id != listed_identity_) {
      listed_identity_ = id;
      listed_.clear();
      if (id) load_listed(path);
    }
    auto it = listed_.find(real);
    if (it == listed_.end()) return std::nullopt;
    compile::Request check = compile::check_command(it->second.args, it->second.directory, real, env_);
    if (compile::sources_of(check).empty()) return std::nullopt;
    return check;
  }

  // The entries of a JSON compilation database: `directory`, `file`, and `arguments` (the
  // compiler first) or `command` (a shell's words). The first entry of a file wins, as in other
  // tools; an entry that cannot be read is skipped.
  void load_listed(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    auto json = lsp::parse(text);
    const lsp::Array* entries = json ? json->array() : nullptr;
    if (!entries) {
      log(std::format("{}: not a JSON array of entries: ignored", path));
      return;
    }
    for (const lsp::Json& e : *entries) {
      const lsp::Json* dir = e["directory"];
      const lsp::Json* file = e["file"];
      if (!dir || !dir->string() || !file || !file->string()) continue;
      std::vector<std::string> args;
      if (const lsp::Json* a = e["arguments"]; a && a->array()) {
        for (const lsp::Json& x : *a->array())
          if (x.string()) args.push_back(*x.string());
      } else if (const lsp::Json* c = e["command"]; c && c->string()) {
        args = compile::split_words(*c->string());
      }
      if (args.empty()) continue;
      args.erase(args.begin());  // the compiler
      // Response files, relative to the entry's directory (as the compiler would read them).
      std::vector<std::string> expanded;
      for (std::string& arg : args) {
        if (!arg.starts_with('@')) {
          expanded.push_back(std::move(arg));
          continue;
        }
        std::filesystem::path rsp(arg.substr(1));
        if (rsp.is_relative()) rsp = std::filesystem::path(*dir->string()) / rsp;
        std::ifstream r(rsp, std::ios::binary);
        if (!r) continue;
        expanded.append_range(compile::split_words(std::string{std::istreambuf_iterator<char>(r), {}}));
      }
      args = std::move(expanded);
      std::filesystem::path f(*file->string());
      if (f.is_relative()) f = std::filesystem::path(*dir->string()) / f;
      listed_.try_emplace(canonical(f.lexically_normal().native_encoded_string()), Listed{std::move(args), *dir->string()});
    }
  }

  // The request that checks the document at `real` (§6.4): its unit in the last served request
  // that compiled it, else its `compile_commands.json` line, else the file alone in its
  // directory; nothing if it is no translation unit.
  std::optional<compile::Request> check_request(const std::string& real) {
    {
      std::lock_guard g(commands_mu_);
      if (auto it = commands_.find(real); it != commands_.end()) return compile::check_one(*it->second.request, it->second.source);
    }
    if (auto listed = listed_request(real)) return listed;
    compile::Request alone{{"-fsyntax-only", real}, std::filesystem::path(real).parent_path().native_encoded_string(), env_, 0};
    if (compile::sources_of(alone).empty()) return std::nullopt;
    return alone;
  }

  // The editor's end of an LSP session: its bytes in, `LspOut` frames out.
  class SocketChannel final : public lsp::Channel {
   public:
    explicit SocketChannel(int fd) : fd_(fd) {}
    std::size_t read(std::span<char> out) override { return read_some(fd_, out); }
    bool pending() override { return wait_readable(fd_, std::chrono::milliseconds(0)); }
    bool write(std::string_view bytes) override { return send(fd_, Kind::lsp, LspOut{std::string(bytes)}); }

   private:
    int fd_;
  };

  void serve_lsp(int fd) {
    {
      std::lock_guard g(conn_mu_);
      if (lsp_closed_) return;  // the daemon is stopping
      lsp_fds_.insert(fd);
    }
    if (send(fd, Kind::lsp, LspRequest{})) {
      SocketChannel io(fd);
      log("an LSP session started");
      const int status =
          lsp::serve(io, [this](std::span<const std::string> paths, const lsp::Buffers& open) { return diagnose(paths, open); },
                     [this](const lsp::Location& at, const lsp::Buffers& open) { return define(at, open); });
      send(fd, Kind::lsp, LspOut{{}, true, status});
      log(std::format("an LSP session ended ({})", status));
    }
    std::lock_guard g(conn_mu_);
    lsp_fds_.erase(fd);
  }

  // Go-to-definition (#170): the document's unit, checked in the editor view with `open` as the
  // editor's buffers (§6.1), asked where the entity named at `at` is first declared.
  std::optional<lsp::Location> define(const lsp::Location& at, const lsp::Buffers& open) {
    const std::string real = canonical(at.path);
    std::optional<compile::Request> request = check_request(real);
    if (!request) return std::nullopt;
    lsp::Buffers spelled;
    for (const auto& [path, text] : open) {
      spelled.insert_or_assign(canonical(path), text);
      spelled.insert_or_assign(path, text);
    }
    std::lock_guard lsp(lsp_mu_);
    {
      std::lock_guard strand(strand_mu_);
      catch_up();
      set_buffers(spelled);
    }
    // A file's text as the editor sees it: its buffer, else the table's.
    auto text_of = [&](const std::string& path) -> std::optional<std::string> {
      if (auto it = spelled.find(path); it != spelled.end()) return it->second;
      return table_.read(path);
    };
    const std::optional<std::string> text = text_of(real);
    if (!text) return std::nullopt;
    pipeline::LineTable lines(*text);
    overlay_.open = &spelled;
    overlay_.cwd = request->cwd;
    struct Reset {
      Overlay& o;
      ~Reset() { o.open = nullptr; }
    } reset{overlay_};
    auto found = compile::definition(*request, overlay_, session(), real, lines.offset_of(at.line, at.column), {pp::View::editor});
    if (!found) return std::nullopt;
    const std::optional<std::string> target = text_of(found->path);
    if (!target) return std::nullopt;
    const pipeline::LineColumn lc = pipeline::LineTable(*target).line_column(found->offset);
    // Under the editor's own name for an open document, as diagnostics are.
    std::string path = found->path;
    for (const auto& [name, _] : open)
      if (canonical(name) == path) path = name;
    return lsp::Location{std::move(path), lc.line, lc.column};
  }

  // The diagnostics of the documents at `paths`, each unit checked in the editor view with `open`
  // as the editor's buffers (§6.1). One freshness batch and one setting of the buffers serve them
  // all.
  std::vector<lsp::Diagnostic> diagnose(std::span<const std::string> paths, const lsp::Buffers& open) {
    struct Check {
      const std::string* path;  // the editor's name
      std::string real;
      compile::Request request;
    };
    std::vector<Check> checks;
    for (const std::string& path : paths) {
      std::string real = canonical(path);
      if (auto r = check_request(real)) checks.push_back({&path, std::move(real), std::move(*r)});
    }
    // Each buffer under the editor's name and its real one: the unit's cells read one or the other,
    // as its command line and includes spell it.
    lsp::Buffers spelled;
    for (const auto& [path, text] : open) {
      spelled.insert_or_assign(canonical(path), text);
      spelled.insert_or_assign(path, text);
    }
    std::lock_guard lsp(lsp_mu_);  // the overlay is one diagnosis's
    {
      std::lock_guard strand(strand_mu_);
      catch_up();
      set_buffers(spelled);
    }
    overlay_.open = &spelled;
    struct Reset {
      Overlay& o;
      ~Reset() { o.open = nullptr; }
    } reset{overlay_};
    std::vector<lsp::Diagnostic> found;
    for (const Check& c : checks) {
      overlay_.cwd = c.request.cwd;
      bool stopped = false;
      auto traced = trace_for(-1, compile::Trace::off, "lsp " + *c.path);
      const compile::Done d =
          compile::run(c.request, overlay_, session(), {}, stopped, {pp::View::editor}, traced ? traced->tracer() : nullptr);
      if (traced) traced->finish();
      {
        std::lock_guard g(stats_mu_);
        ++stats_.diagnoses;
        stats_.executed += d.report.executed;
        stats_.validated += d.report.validated;
        stats_.last_executed = d.report.executed;
      }
      for (lsp::Diagnostic& x : lsp::parse_messages(d.err, c.request.cwd)) {
        // About the unit as a whole, or in it: under the editor's name for it.
        if (x.path.empty() || x.path == c.real || canonical(x.path) == c.real) x.path = *c.path;
        found.push_back(std::move(x));
      }
    }
    return found;
  }

  // The editor's buffers become the `pp::Buffer` inputs (§6.1): an open one set, a closed one
  // cleared. An unchanged one keeps its value, so nothing that read it turns red. A change stops
  // and restarts the running requests (§4.1). On the strand.
  void set_buffers(const lsp::Buffers& open) {
    std::vector<std::pair<std::string, std::optional<std::string>>> sets;
    for (const std::string& b : buffers_)
      if (!open.contains(b)) sets.emplace_back(b, std::nullopt);
    for (const auto& [path, text] : open) {
      auto it = buffer_texts_.find(path);
      if (it == buffer_texts_.end() || it->second != text) sets.emplace_back(path, text);
    }
    if (sets.empty()) return;
    restarted(session().apply([&](engine::Db& db) {
      for (const auto& [path, text] : sets) db.set<pp::Buffer>(pp::path_id(path), text);
    }));
    for (const auto& [path, text] : sets) {
      if (text) {
        buffers_.insert(path);
        buffer_texts_.insert_or_assign(path, *text);
      } else {
        buffers_.erase(path);
        buffer_texts_.erase(path);
      }
    }
  }

  // The admission quantum after a batch stopped running requests (§4.2): it doubles when one of
  // them had stored no cell since its previous start. On the strand.
  void restarted(const engine::Epochs::Applied& a) {
    if (a.respawned > 0) admission_.restarted(a.starved == 0);
  }

  // One request, spawned into the engine's open epoch (§4.3); nothing if the client hung up.
  // Requests run at once: a batch that arrives meanwhile is applied once the quantum allows, and
  // restarts every running request where it stopped (§4.1).
  std::optional<compile::Done> run_compile(int fd, const compile::Request& r, RequestTrace* traced) {
    compile::Report report;
    table_.set_cwd(r.cwd);  // for this thread's reads (`-E`)
    {
      std::lock_guard strand(strand_mu_);
      report.inputs_changed += catch_up();
      std::lock_guard g(stats_mu_);
      report.started_at = stats_.revision;  // what `--watch` waits past: never a batch it missed
    }
    struct Running {
      Server& s;
      explicit Running(Server& s) : s(s) {
        std::lock_guard g(s.strand_mu_);
        ++s.running_;
      }
      ~Running() {
        std::lock_guard g(s.strand_mu_);
        --s.running_;
      }
    } running{*this};
    bool cancelled = false;
    compile::Stop tick = [&] {
      if (hung_up(fd)) return cancelled = true;
      if (traced) traced->pump(), traced->flush_client();  // the trace so far, from this thread (never a worker's)
      if (watcher_) {
        std::unique_lock strand(strand_mu_, std::try_to_lock);
        if (strand && admission_.may_apply(Clock::now())) {
          if (const std::size_t changed = apply_batch(false); changed > 0) {
            report.inputs_changed += changed;
            admission_.applied(Clock::now());
          }
        }
      }
      return false;
    };
    bool stopped = false;
    compile::Done d = compile::run(r, table_, session(), tick, stopped, {}, traced ? traced->tracer() : nullptr);
    if (stopped) {
      log("a client hung up: its request stopped");
      return std::nullopt;
    }
    std::uint64_t quantum = 0;
    {
      // The quantum returns to the floor once every live root has stored its answer (§4.2): when
      // the last running request completes. One finishing beside a starved one does not.
      std::lock_guard strand(strand_mu_);
      if (running_ == 1) admission_.progressed();
      quantum = static_cast<std::uint64_t>(admission_.quantum().count());
    }
    std::lock_guard g(stats_mu_);
    d.report.revision = stats_.revision;
    d.report.started_at = report.started_at;
    d.report.inputs_changed = report.inputs_changed;
    stats_.quantum_ms = quantum;
    stats_.entries = table_.size();
    return d;
  }

  // Freshness (§5.4): every change made before the request was sent is queued, then applied as a
  // batch; the entries it changed. On the strand.
  std::size_t catch_up() {
    bool rescan = false;
    if (watcher_) {
      renew_cookie_dir();
      watcher_->refresh();
      if (!events_->sync(o_.cookie_timeout)) {
        rescan = true;
        std::lock_guard g(stats_mu_);
        ++stats_.cookie_timeouts;
      }
    }
    const std::size_t changed = apply_batch(rescan);
    admission_.applied(Clock::now());
    return changed;
  }

  // The engine session every request runs in (#63), for the daemon's life: its file inputs are
  // provided from the table (§5.1), and a batch sets the ones it changed, so an edit invalidates
  // exactly the cells that read the file and a repeated request executes nothing.
  pipeline::Session& session() {
    std::call_once(session_once_, [this] {
      pool_ = std::make_unique<engine::Pool>(threads_.count, threads_.chaos_seed);
      session_ = std::make_unique<pipeline::Session>(*pool_, o_.build_checkpoint);
      session_->db().set_provider<pp::FileContent>([this](pp::PathId id) { return provide(id); });
      session_->set_reader([this](const std::string& path) { return table_.read(path); });
      build_ = std::make_unique<build::Coordinator>(*session_, executable_path(), build_hash());
    });
    return *session_;
  }

  // The toolchain and sysroot never change under a build: their files are `high` (§5.1).
  engine::provided<std::optional<std::string>> provide(pp::PathId id) {
    const std::string& key = pp::path_of(id);
    {
      std::lock_guard g(provided_mu_);
      provided_.insert(key);
    }
    bool system = std::ranges::any_of(system_dirs_, [&](const std::string& d) {
      return key.size() > d.size() && key.starts_with(d) && key[d.size()] == '/';
    });
    return {table_.read(key), system ? engine::Durability::high : engine::Durability::low};
  }

  // The provided inputs whose entries a batch changed get their new values, and the main files of
  // the units named so far theirs: at quiescence, stopping and restarting the running requests
  // (§4.1). On the strand.
  void set_changed_inputs() {
    std::vector<std::string> changed = table_.take_changed();
    if (changed.empty()) return;
    std::vector<std::string> provided;
    {
      std::lock_guard g(provided_mu_);
      for (const std::string& key : changed)
        if (provided_.contains(key)) provided.push_back(key);
    }
    restarted(session().apply([&](engine::Db& db) {
      for (const std::string& key : provided) db.set<pp::FileContent>(pp::path_id(key), table_.read(key));
      for (const std::string& key : changed) session_->set_main_file(key, table_.read(key));
    }));
  }

  ServerOptions o_;
  Paths paths_;
  std::vector<std::pair<std::string, std::string>> env_;  // the configuration this daemon serves
  Fd lock_;
  Fd listener_;
  Fd wake_r_, wake_w_;
  std::optional<Events> events_;
  std::optional<Watcher> watcher_;
  std::string watcher_state_;
  FileTable table_;
  Admission admission_;
  // The edit strand (§4.1): batches, the editor's buffers and the admission quantum, one at a
  // time. Requests run concurrently, outside it.
  std::mutex strand_mu_;
  std::mutex lsp_mu_;  // one diagnosis at a time: the overlay is its
  const compile::Threads threads_ = compile::threads();
  std::once_flag session_once_;
  std::unique_ptr<engine::Pool> pool_;           // outlives the session
  std::unique_ptr<pipeline::Session> session_;  // created once, for the daemon's life
  std::unique_ptr<build::Coordinator> build_;
  std::mutex provided_mu_;
  std::set<std::string> provided_;  // the table keys provided to session_'s FileContent cells
  std::vector<std::string> system_dirs_;

  // The editor view's host outside the engine (the unit's own text, which the driver reads and
  // renders messages against): an open buffer, else the table (§6.1).
  class Overlay final : public pp::Host {
   public:
    explicit Overlay(FileTable& t) : table_(&t) {}
    std::optional<std::string> read(const std::string& path) override {
      if (open) {
        std::filesystem::path p(path);
        if (p.is_relative() && !cwd.empty()) p = std::filesystem::path(cwd) / p;
        if (auto it = open->find(p.lexically_normal().generic_native_encoded_string()); it != open->end()) return it->second;
      }
      return table_->read(path);
    }
    const lsp::Buffers* open = nullptr;  // during a diagnosis
    std::string cwd;

   private:
    FileTable* table_;
  };
  Overlay overlay_{table_};                     // under lsp_mu_
  std::set<std::string> buffers_;               // the paths whose `pp::Buffer` is set; on the strand
  std::map<std::string, std::string> buffer_texts_;  // their texts; on the strand
  struct Command {
    std::shared_ptr<const compile::Request> request;
    std::string source;  // as `request` names it, absolute
  };
  std::mutex commands_mu_;
  struct Listed {
    std::vector<std::string> args;  // without the compiler
    std::string directory;
  };
  std::mutex listed_mu_;
  std::optional<Identity> listed_identity_;   // of compile_commands.json when last read; under listed_mu_
  std::map<std::string, Listed> listed_;      // by real path; under listed_mu_
  std::map<std::string, Command> commands_;  // real path -> the last served request compiling it
  std::set<int> lsp_fds_;    // the LSP sessions' connections; under conn_mu_
  bool lsp_closed_ = false;  // stopping: no new session; under conn_mu_

  std::mutex conn_mu_;
  std::condition_variable conn_cv_;
  std::size_t connections_ = 0;
  Clock::time_point last_activity_;
  std::atomic<bool> shutdown_requested_{false};

  std::mutex stats_mu_;
  Status stats_;

  std::mutex watch_mu_;
  std::vector<std::shared_ptr<Watching>> watchers_;  // under watch_mu_
  std::atomic<std::uint64_t> trace_requests_{0};
  const Clock::time_point trace_origin_ = Clock::now();  // the clock of every trace's events
  // This start of the daemon, for `--watch` (a pid may be reused by a later daemon).
  const std::uint64_t instance_ = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) ^
                                  (static_cast<std::uint64_t>(my_pid()) << 40);
  std::uint64_t running_ = 0;  // compile requests admitted and not yet answered; on the strand
};

}  // namespace

int serve(const ServerOptions& o) {
  auto dir = ensure_private_dir(runtime_dir());
  if (!dir) {
    log(dir.error());
    return 1;
  }
  Server s(o, paths_for(o.root, *dir));
  return s.run();
}

}  // namespace ycxx::daemon
