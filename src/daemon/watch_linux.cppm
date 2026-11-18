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

  std::optional<std::string> start(std::vector<std::string> dirs) {
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

  void add(const std::string& dir) {
    std::lock_guard g(mu_);
    if (watched_.contains(dir) || unwatched_.contains(dir)) return;
    int wd = ::inotify_add_watch(fd_.get(), dir.c_str(), mask);
    if (wd < 0) {
      if (errno == ENOSPC) unwatched_.insert(dir);
      return;
    }
    dirs_[wd] = dir;
    watched_.insert(dir);
  }

  void refresh() {}

  std::vector<std::string> unwatched() const {
    std::lock_guard g(mu_);
    return {unwatched_.begin(), unwatched_.end()};
  }

 private:
  static constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                                        IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

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
            if (e->mask & IN_IGNORED) {
              // The directory is gone: its entries move their watch to an ancestor when the
              // batch re-reads them.
              watched_.erase(it->second);
              dirs_.erase(it);
            }
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
  std::set<std::string> watched_;
  std::set<std::string> unwatched_;
};

}  // namespace ycxx::daemon
