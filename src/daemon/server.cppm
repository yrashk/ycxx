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
export module ycxx.daemon:server;

import std;
import ycxx.compile;
import ycxx.engine;
import ycxx.lsp;
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
    // An LSP session lasts as long as its editor: it is ended, and its `ycxx lsp` exits.
    listener_.reset();
    remove_file(paths_.socket());
    std::unique_lock g(conn_mu_);
    lsp_closed_ = true;
    for (int fd : lsp_fds_) shutdown_both(fd);
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
        serve_lsp(fd);
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
  // document is checked with it (§6.4; `compile_commands.json` is the fallback still to come,
  // #167). One copy of the request serves all its sources.
  void remember(const compile::Request& r) {
    auto shared = std::make_shared<const compile::Request>(r);
    std::vector<std::pair<std::string, std::string>> sources;
    for (std::string& s : compile::sources_of(r)) sources.emplace_back(canonical(s), std::move(s));
    std::lock_guard g(commands_mu_);
    for (auto& [real, spelled] : sources) commands_.insert_or_assign(std::move(real), Command{shared, std::move(spelled)});
  }

  // The request that checks the document at `real`: its unit in the last served request that
  // compiled it, else the file alone in its directory; nothing if it is no translation unit.
  std::optional<compile::Request> check_request(const std::string& real) {
    {
      std::lock_guard g(commands_mu_);
      if (auto it = commands_.find(real); it != commands_.end()) return compile::check_one(*it->second.request, it->second.source);
    }
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
      const int status = lsp::serve(io, [this](std::span<const std::string> paths, const lsp::Buffers& open) { return diagnose(paths, open); });
      send(fd, Kind::lsp, LspOut{{}, true, status});
      log(std::format("an LSP session ended ({})", status));
    }
    std::lock_guard g(conn_mu_);
    lsp_fds_.erase(fd);
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
    std::unique_lock engine(engine_mu_);
    catch_up();
    set_buffers(spelled);
    overlay_.open = &spelled;
    struct Reset {
      Overlay& o;
      ~Reset() { o.open = nullptr; }
    } reset{overlay_};
    std::vector<lsp::Diagnostic> found;
    for (const Check& c : checks) {
      table_.set_cwd(c.request.cwd);
      overlay_.cwd = c.request.cwd;
      bool stopped = false;
      const compile::Done d = compile::run(c.request, overlay_, session(), {}, stopped, {pp::View::editor, editor_unit(c.real)});
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
  // cleared. An unchanged one keeps its value, so nothing that read it turns red. Under engine_mu_.
  void set_buffers(const lsp::Buffers& open) {
    engine::Db& db = session().db();
    for (auto it = buffers_.begin(); it != buffers_.end();) {
      if (open.contains(*it)) {
        ++it;
        continue;
      }
      db.set<pp::Buffer>(pp::path_id(*it), std::nullopt);
      it = buffers_.erase(it);
    }
    for (const auto& [path, text] : open) {
      db.set<pp::Buffer>(pp::path_id(path), text);
      buffers_.insert(path);
    }
  }

  // An editor-view unit's number, one per document for the daemon's life, apart from every
  // disk-view number (a build numbers its units from 0).
  std::uint32_t editor_unit(const std::string& path) {
    auto [it, _] = editor_units_.try_emplace(path, (1u << 31) + static_cast<std::uint32_t>(editor_units_.size()));
    return it->second;
  }

  // One request, admitted into the engine; nothing if the client hung up.
  std::optional<compile::Done> run_compile(int fd, const compile::Request& r) {
    std::unique_lock engine(engine_mu_, std::defer_lock);
    while (!engine.try_lock_for(std::chrono::milliseconds(50)))
      if (hung_up(fd)) return std::nullopt;

    compile::Report report;
    report.inputs_changed += catch_up();
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

  // Freshness (§5.4): every change made before the request was sent is queued, then applied as a
  // batch; the entries it changed. Under engine_mu_.
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
    if (!session_) {
      if (!pool_) pool_ = std::make_unique<engine::Pool>(threads_.count, threads_.chaos_seed);
      session_ = std::make_unique<pipeline::Session>(*pool_);
      provided_.clear();
      buffers_.clear();
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
  const compile::Threads threads_ = compile::threads();
  std::unique_ptr<engine::Pool> pool_;           // under engine_mu_; outlives the session
  std::unique_ptr<pipeline::Session> session_;  // under engine_mu_
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
  Overlay overlay_{table_};                          // under engine_mu_
  std::set<std::string> buffers_;                    // the paths whose `pp::Buffer` is set; under engine_mu_
  std::map<std::string, std::uint32_t> editor_units_;  // under engine_mu_
  struct Command {
    std::shared_ptr<const compile::Request> request;
    std::string source;  // as `request` names it, absolute
  };
  std::mutex commands_mu_;
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
