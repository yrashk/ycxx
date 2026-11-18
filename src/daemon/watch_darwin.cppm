// The macOS watcher (daemon design §5.5): one recursive FSEvents stream with file-level events
// over the scope root, the runtime directory, the toolchain and sysroot, and every directory
// outside them that holds a table entry. A stream's paths are fixed, so a new directory is added
// by recreating the stream from the event id taken before the first file in it was read: FSEvents
// replays what happened since, and nothing between the read and the new stream is lost. CMake
// picks this file or watch_linux.cppm.
module;
#include <FSEvents/FSEvents.h>
#include <dispatch/dispatch.h>
export module ycxx.daemon:watch;

import std;
import :events;

export namespace ycxx::daemon {

class Watcher {
 public:
  explicit Watcher(Events& events) : events_(&events) {}
  Watcher(const Watcher&) = delete;
  ~Watcher() { stop(); }

  std::string_view name() const { return "fsevents"; }

  // Starts the stream over `dirs` (real paths); why not, on failure.
  std::optional<std::string> start(std::vector<std::string> dirs) {
    std::lock_guard g(mu_);
    for (std::string& d : dirs) cover(std::move(d));
    queue_ = ::dispatch_queue_create("ycxx.watch", nullptr);
    if (!create(kFSEventStreamEventIdSinceNow)) return "cannot start an FSEvents stream";
    return std::nullopt;
  }

  // A directory holding a table entry (a real path). From the compiling thread, before the read.
  void add(const std::string& dir) {
    std::lock_guard g(mu_);
    if (covered(dir)) return;
    if (!since_) since_ = ::FSEventsGetCurrentEventId();
    cover(dir);
    dirty_roots_ = true;
  }

  // Between epochs, before a cookie: recreates the stream if directories were added.
  void refresh() {
    std::lock_guard g(mu_);
    if (!dirty_roots_) return;
    dirty_roots_ = false;
    FSEventStreamEventId from = std::min<FSEventStreamEventId>(*since_, last_id_.load());
    since_.reset();
    destroy();
    if (!create(from)) events_->overflow();
  }

  // Directories checked per request instead of watched (§5.5); FSEvents has no limit.
  std::vector<std::string> unwatched() const { return {}; }

 private:
  static bool beneath_or_at(std::string_view path, std::string_view dir) {
    return path == dir || (path.starts_with(dir) && path.size() > dir.size() && (path[dir.size()] == '/' || dir == "/"));
  }
  bool covered(std::string_view dir) const {
    return std::ranges::any_of(roots_, [&](const std::string& r) { return beneath_or_at(dir, r); });
  }
  void cover(std::string dir) {
    if (covered(dir)) return;
    std::erase_if(roots_, [&](const std::string& r) { return beneath_or_at(r, dir); });
    roots_.push_back(std::move(dir));
  }

  static void callback(ConstFSEventStreamRef, void* info, std::size_t n, void* paths, const FSEventStreamEventFlags flags[],
                       const FSEventStreamEventId ids[]) {
    auto* self = static_cast<Watcher*>(info);
    auto** names = static_cast<char**>(paths);
    for (std::size_t i = 0; i < n; ++i) {
      FSEventStreamEventId seen = self->last_id_.load();
      while (ids[i] > seen && !self->last_id_.compare_exchange_weak(seen, ids[i])) {
      }
      if (flags[i] & kFSEventStreamEventFlagHistoryDone) continue;
      if (flags[i] & (kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) {
        self->events_->overflow();
        continue;
      }
      // MustScanSubDirs names a directory whose events were coalesced: every entry beneath it is
      // dirty, which a directory path already means.
      self->events_->push(names[i]);
    }
  }

  bool create(FSEventStreamEventId since) {
    std::vector<CFStringRef> strings;
    for (const std::string& r : roots_) strings.push_back(::CFStringCreateWithCString(nullptr, r.c_str(), kCFStringEncodingUTF8));
    CFArrayRef array = ::CFArrayCreate(nullptr, reinterpret_cast<const void**>(strings.data()), static_cast<CFIndex>(strings.size()),
                                       &kCFTypeArrayCallBacks);
    for (CFStringRef s : strings) ::CFRelease(s);
    FSEventStreamContext ctx{0, this, nullptr, nullptr, nullptr};
    if (since == kFSEventStreamEventIdSinceNow) last_id_ = ::FSEventsGetCurrentEventId();
    stream_ = ::FSEventStreamCreate(nullptr, &callback, &ctx, array, since, 0.005,
                                    kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    ::CFRelease(array);
    if (!stream_) return false;
    ::FSEventStreamSetDispatchQueue(stream_, queue_);
    if (!::FSEventStreamStart(stream_)) {
      destroy();
      return false;
    }
    return true;
  }
  void destroy() {
    if (!stream_) return;
    ::FSEventStreamStop(stream_);
    ::FSEventStreamInvalidate(stream_);
    ::FSEventStreamRelease(stream_);
    stream_ = nullptr;
  }
  void stop() {
    std::lock_guard g(mu_);
    destroy();
  }

  Events* events_;
  std::mutex mu_;
  std::vector<std::string> roots_;
  bool dirty_roots_ = false;
  std::optional<FSEventStreamEventId> since_;
  std::atomic<FSEventStreamEventId> last_id_{0};
  dispatch_queue_t queue_ = nullptr;
  FSEventStreamRef stream_ = nullptr;
};

}  // namespace ycxx::daemon
