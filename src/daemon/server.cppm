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
export module ycxx.daemon:server;

import std;
import ycxx.compile;
import ycxx.pipeline;
import ycxx.pp;
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
};

// Runs the daemon until idle exit, `shutdown` or SIGTERM; the process's exit status.
int serve(const ServerOptions& o);

}  // namespace ycxx::daemon

namespace ycxx::daemon {
namespace {

using Clock = std::chrono::steady_clock;

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
    for (const std::string& d : pp::system_dirs())
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
    listener_.reset();
    remove_file(paths_.socket());
    std::unique_lock g(conn_mu_);
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
      for (const std::string& d : pp::system_dirs())
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
    std::thread([this, c = std::move(c)]() mutable {
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
    if (!send(fd, Kind::welcome, Welcome{my_pid(), paths_.root})) return;
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
        send(fd, Kind::refused, Refused{"the LSP front is not implemented yet (#99)"});
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

  void serve_compile(int fd, const compile::Request& r) {
    if (r.env != env_) {
      send(fd, Kind::refused, Refused{"the environment differs from the daemon's"});
      return;
    }
    {
      std::lock_guard g(stats_mu_);
      ++stats_.live;
    }
    auto done = run_compile(fd, r);
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
    if (done) send(fd, Kind::done, *done);
  }

  // One request, admitted into the engine; nothing if the client hung up.
  std::optional<compile::Done> run_compile(int fd, const compile::Request& r) {
    std::unique_lock engine(engine_mu_, std::defer_lock);
    while (!engine.try_lock_for(std::chrono::milliseconds(50)))
      if (hung_up(fd)) return std::nullopt;

    // Freshness (§5.4): every change made before the request was sent is queued, then applied.
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
    compile::Report report;
    report.inputs_changed += apply_batch(rescan);
    admission_.applied(Clock::now());
    table_.set_cwd(r.cwd);

    for (;;) {
      bool cancelled = false;
      Events::Batch pending;
      compile::Stop stop = [&] {
        if (hung_up(fd)) return cancelled = true;
        // A batch that reaches the table stops the request, once the quantum allows (§4.2).
        Events::Batch more = events_->drain();
        pending.paths.append_range(more.paths);
        pending.overflow = pending.overflow || more.overflow;
        if ((pending.overflow || table_.knows_any(pending.paths)) && admission_.may_apply(Clock::now())) return true;
        return false;
      };
      bool stopped = false;
      compile::Done d = compile::run(r, table_, session(), stop, stopped);
      if (!stopped) {
        // The only live root stored its answer: the quantum returns to the floor (§4.2).
        admission_.progressed();
        requeue(std::move(pending));
        std::lock_guard g(stats_mu_);
        report.revision = stats_.revision;
        d.report.restarts = report.restarts;
        d.report.revision = report.revision;
        d.report.inputs_changed = report.inputs_changed;
        stats_.quantum_ms = static_cast<std::uint64_t>(admission_.quantum().count());
        stats_.entries = table_.size();
        return d;
      }
      if (cancelled) {
        requeue(std::move(pending));
        log("a client hung up: its request stopped");
        return std::nullopt;
      }
      // Restart at the new revision: what was not yet applied goes back in the queue and is
      // applied as the next batch.
      requeue(std::move(pending));
      // A restart runs the request from its first unit, so nothing it finished survives: it never
      // counts as progress, and the quantum doubles until it outlasts a whole run (§4.2). With
      // cells that survive restarts (#120) progress becomes "stored a cell".
      admission_.restarted(false);
      report.inputs_changed += apply_batch(false);
      admission_.applied(Clock::now());
      ++report.restarts;
    }
  }

  // The engine session every request runs in (#63), for the daemon's life: its file inputs are
  // provided from the table (§5.1), and a batch sets the ones it changed, so an edit invalidates
  // exactly the cells that read the file and a repeated request executes nothing.
  pipeline::Session& session() {
    if (!session_) {
      session_ = std::make_unique<pipeline::Session>();
      provided_.clear();
      session_->db().set_provider<pp::FileContent>([this](pp::PathId id) { return provide(id); });
    }
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

  // At quiescence: the provided inputs whose entries a batch changed get their new values.
  void set_changed_inputs() {
    std::vector<std::string> changed = table_.take_changed();
    if (!session_) return;
    std::lock_guard g(provided_mu_);
    for (const std::string& key : changed)
      if (provided_.contains(key)) session_->db().set<pp::FileContent>(pp::path_id(key), table_.read(key));
  }

  void requeue(Events::Batch b) {
    for (std::string& p : b.paths) events_->push(std::move(p));
    if (b.overflow) events_->overflow();
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
  std::timed_mutex engine_mu_;  // one request in the engine at a time (#15 lifts this)
  std::unique_ptr<pipeline::Session> session_;  // under engine_mu_
  std::mutex provided_mu_;
  std::set<std::string> provided_;  // the table keys provided to session_'s FileContent cells
  std::vector<std::string> system_dirs_;

  std::mutex conn_mu_;
  std::condition_variable conn_cv_;
  std::size_t connections_ = 0;
  Clock::time_point last_activity_;
  std::atomic<bool> shutdown_requested_{false};

  std::mutex stats_mu_;
  Status stats_;
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
