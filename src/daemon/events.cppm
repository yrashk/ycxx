// What the watcher delivers (daemon design §5.3, §5.4): dirty paths with no payload, overflow, and
// the cookies that show every earlier event has been queued. The watcher's thread pushes; the
// daemon drains between epochs. Platform watchers (:watch) only push real paths.
export module ycxx.daemon:events;

import std;
import :posix;

export namespace ycxx::daemon {

class Events {
 public:
  // Cookies are files `cookie-<pid>-<seq>` in `dir` (a real path on the watched volume).
  explicit Events(std::string cookie_dir) : cookie_dir_(std::move(cookie_dir)), prefix_(std::format("cookie-{}-", my_pid())) {}

  // From the watcher's thread: a path (real, no trailing slash) changed. A cookie of this daemon
  // is recognized here; other paths in the cookie directory are kept and dropped later like any
  // path no cell knows.
  void push(std::string path) {
    while (path.size() > 1 && path.ends_with('/')) path.pop_back();
    std::lock_guard g(mu_);
    if (auto seq = cookie_of(path)) {
      seen_ = std::max(seen_, *seq);
      in_flight_ = in_flight_ && seen_ < issued_;
      cv_.notify_all();
      return;
    }
    if (overflow_) return;
    // A queue drained only when a request arrives must not grow without bound while the daemon
    // is idle (a checkout, `npm install`): past the cap the next epoch rescans instead.
    if (dirty_.size() >= max_dirty) {
      dirty_.clear();
      overflow_ = true;
      return;
    }
    dirty_.insert(std::move(path));
  }
  // Events were lost (inotify's IN_Q_OVERFLOW, FSEvents' MustScanSubDirs or a drop): the next
  // epoch rescans the table.
  void overflow() {
    std::lock_guard g(mu_);
    overflow_ = true;
  }

  struct Batch {
    std::vector<std::string> paths;
    bool overflow = false;
  };
  Batch drain() {
    std::lock_guard g(mu_);
    Batch b{{std::make_move_iterator(dirty_.begin()), std::make_move_iterator(dirty_.end())}, overflow_};
    dirty_.clear();
    overflow_ = false;
    return b;
  }

  // Cookie sync (§5.4): creates a cookie and waits until the watcher has seen it, so every change
  // made before this call is queued. A request arriving while a cookie is in flight waits for the
  // next one, which is shared by every request that arrived meanwhile. False on timeout or if the
  // cookie cannot be written.
  bool sync(std::chrono::milliseconds timeout) {
    std::unique_lock g(mu_);
    // A cookie already in flight may have been written before this request was sent: wait for the
    // next one.
    const std::uint64_t need = issued_ + 1;
    auto until = std::chrono::steady_clock::now() + timeout;
    while (seen_ < need) {
      // No cookie in flight that this request may use: write the one it needs, shared by every
      // request that arrives before it is seen.
      if (!in_flight_ && issued_ < need && !issue()) return false;
      if (cv_.wait_until(g, until) == std::cv_status::timeout && seen_ < need) {
        in_flight_ = false;
        return false;
      }
    }
    return true;
  }

  const std::string& cookie_dir() const { return cookie_dir_; }

 private:
  std::optional<std::uint64_t> cookie_of(std::string_view path) const {
    if (!path.starts_with(cookie_dir_) || path.size() <= cookie_dir_.size() || path[cookie_dir_.size()] != '/') return std::nullopt;
    std::string_view name = path.substr(cookie_dir_.size() + 1);
    if (!name.starts_with(prefix_)) return std::nullopt;
    std::uint64_t seq = 0;
    auto rest = name.substr(prefix_.size());
    auto [p, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), seq);
    if (ec != std::errc{} || p != rest.data() + rest.size()) return std::nullopt;
    return seq;
  }
  // Under mu_: writes the next cookie and removes it at once; either event marks it seen.
  bool issue() {
    std::string path = std::format("{}/{}{}", cookie_dir_, prefix_, issued_ + 1);
    if (!create_new_file(path, "")) return false;
    remove_file(path);
    ++issued_;
    in_flight_ = true;
    return true;
  }

  std::string cookie_dir_;
  std::string prefix_;
  std::mutex mu_;
  std::condition_variable cv_;
  static constexpr std::size_t max_dirty = 100'000;
  std::set<std::string> dirty_;
  bool overflow_ = false;
  std::uint64_t issued_ = 0;
  std::uint64_t seen_ = 0;
  bool in_flight_ = false;
};

}  // namespace ycxx::daemon
