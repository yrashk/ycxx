// The Linux watcher (daemon design §5.5): one inotify descriptor with one watch per directory
// that holds a table entry, positive or negative (the nearest existing ancestor of a missing
// one), read by its own thread. A watch is added before the first file in its directory is read,
// so a change after the read is always seen; at `max_user_watches` the directory is stat-checked
// per request instead. CMake picks this file or watch_darwin.cppm.
module;
#include <errno.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
export module ycxx.daemon:watch;

import std;
import :events;
import :posix;

export namespace ycxx::daemon {

class Watcher {
 public:
  explicit Watcher(Events& events) : events_(&events) {}
  Watcher(const Watcher&) = delete;
  ~Watcher() {
    if (stop_w_) {
      char c = 's';
      [[maybe_unused]] auto n = ::write(stop_w_.get(), &c, 1);
    }
    if (thread_.joinable()) thread_.join();
  }

  std::string_view name() const { return "inotify"; }

  // `root` is the scope's; inotify watches are per directory, so none is too wide.
  std::optional<std::string> start(std::vector<std::string> dirs, const std::string& /*root*/) {
    fd_ = Fd(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
    if (!fd_) return os_error("inotify_init1");
    auto p = make_pipe();
    if (!p) return p.error();
    stop_r_ = std::move(p->first);
    stop_w_ = std::move(p->second);
    for (const std::string& d : dirs) add(d);
    thread_ = std::thread([this] { loop(); });
    return std::nullopt;
  }

  // A directory holding a table entry. A path watched before is watched again when it is now
  // another directory (renamed away, or deleted and created again before the watcher thread saw
  // it). A directory that cannot be watched is stat-checked per request; one refused for another
  // reason than the watch limit is tried again on its next read.
  void add(const std::string& dir) {
    std::lock_guard g(mu_);
    auto id = stat_path(dir);
    if (auto it = watched_.find(dir); it != watched_.end()) {
      if (id && it->second.dev == id->dev && it->second.ino == id->ino) return;
      forget(it->second.wd);
    }
    if (limited_.contains(dir)) return;
    int wd = ::inotify_add_watch(fd_.get(), dir.c_str(), mask);
    if (wd < 0) {
      unwatched_.insert(dir);
      if (errno == ENOSPC) limited_.insert(dir);
      return;
    }
    unwatched_.erase(dir);
    dirs_[wd] = dir;
    watched_[dir] = Watch{wd, id ? id->dev : 0, id ? id->ino : 0};
  }

  void refresh() {}
  // The cookie directory was created again: watch it again.
  void renew(const std::string& dir) { add(dir); }

  std::vector<std::string> unwatched() const {
    std::lock_guard g(mu_);
    return {unwatched_.begin(), unwatched_.end()};
  }

 private:
  static constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                                        IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

  struct Watch {
    int wd = -1;
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
  };

  // Under mu_: drops a watch descriptor and the path it stood for.
  void forget(int wd) {
    auto it = dirs_.find(wd);
    if (it == dirs_.end()) return;
    if (auto w = watched_.find(it->second); w != watched_.end() && w->second.wd == wd) watched_.erase(w);
    dirs_.erase(it);
    ::inotify_rm_watch(fd_.get(), wd);
  }

  void loop() {
    alignas(inotify_event) std::array<char, 64 * 1024> buf;
    for (;;) {
      pollfd ps[2] = {{fd_.get(), POLLIN, 0}, {stop_r_.get(), POLLIN, 0}};
      if (::poll(ps, 2, -1) < 0) {
        if (errno == EINTR) continue;
        events_->overflow();
        return;
      }
      if (ps[1].revents) return;
      for (;;) {
        ssize_t n = ::read(fd_.get(), buf.data(), buf.size());
        if (n <= 0) break;
        for (char* p = buf.data(); p < buf.data() + n;) {
          auto* e = reinterpret_cast<inotify_event*>(p);
          p += sizeof(inotify_event) + e->len;
          if (e->mask & IN_Q_OVERFLOW) {
            events_->overflow();
            continue;
          }
          std::string path;
          {
            std::lock_guard g(mu_);
            auto it = dirs_.find(e->wd);
            if (it == dirs_.end()) continue;
            path = it->second;
            // The directory is gone or moved away, and the descriptor would report the moved
            // directory's events under the old path: its entries are re-read by this event's
            // batch, which watches whatever directory now has the path, or an ancestor.
            if (e->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) forget(e->wd);
          }
          if (e->len > 0) path = (path == "/" ? "" : path) + "/" + e->name;
          events_->push(std::move(path));
        }
      }
    }
  }

  Events* events_;
  Fd fd_;
  Fd stop_r_;
  Fd stop_w_;
  std::thread thread_;
  mutable std::mutex mu_;
  std::map<int, std::string> dirs_;
  std::map<std::string, Watch> watched_;
  std::set<std::string> unwatched_;
  std::set<std::string> limited_;  // refused at max_user_watches
};

}  // namespace ycxx::daemon
