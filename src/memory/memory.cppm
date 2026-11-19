// What the process allocates (#274): the program's replacements of the global allocation
// functions (`new.cpp`) count every allocation and free here, into process totals and into the
// current thread's target, the `Usage` of whatever engine frame runs on it. The engine sets the
// target where a frame's code runs and clears it where another worker may take the frame over, so
// a frame's numbers follow it across workers (engine.cppm, "Memory").
//
// Sizes are the requested ones; a sized delete gives them back exactly, and an unsized delete
// (rare: 42 of 171,171 frees compiling <stdio.h>) asks the platform for the block's usable size.
// Process totals are counted per thread and added to the process's atomics once a thread has
// moved `batch` bytes, or when it exits, so they are exact within `batch` bytes per thread.
// Numbers vary by host and run: they never enter a value, a fingerprint or a comparison of runs.
export module ycxx.memory;

export import :usable;
import std;

export namespace ycxx::memory {

// One frame's allocations while it ran (or anything else that takes the target): bytes and count
// allocated, net bytes (allocated minus freed), and the net's high-water mark. Written by the one
// thread running the frame at a time; atomics, so a reader on another thread (or a free in the
// short windows a frame changes hands) is never a data race.
struct Usage {
  std::atomic<std::uint64_t> allocated{0};
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::int64_t> net{0};
  std::atomic<std::int64_t> peak{0};

  // `inline`: a member defined in its class is not implicitly inline in a named module.
  inline void add(std::uint64_t n) noexcept {
    allocated.store(allocated.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    const std::int64_t v = net.load(std::memory_order_relaxed) + static_cast<std::int64_t>(n);
    net.store(v, std::memory_order_relaxed);
    if (v > peak.load(std::memory_order_relaxed)) peak.store(v, std::memory_order_relaxed);
  }
  inline void sub(std::uint64_t n) noexcept {
    net.store(net.load(std::memory_order_relaxed) - static_cast<std::int64_t>(n), std::memory_order_relaxed);
  }
};

// A `Usage` read at one moment.
struct Snapshot {
  std::uint64_t allocated = 0;
  std::uint64_t count = 0;
  std::int64_t net = 0;
  std::int64_t peak = 0;
};
Snapshot snapshot(const Usage& u) noexcept {
  return {u.allocated.load(std::memory_order_relaxed), u.count.load(std::memory_order_relaxed), u.net.load(std::memory_order_relaxed),
          u.peak.load(std::memory_order_relaxed)};
}

// The process: bytes allocated now, their high-water mark, and bytes and allocations ever made.
struct Totals {
  std::int64_t current = 0;
  std::int64_t peak = 0;
  std::uint64_t allocated = 0;
  std::uint64_t count = 0;
};

// A thread's counts not yet added to the process's, and its target. `budget` falls by every
// size counted; at zero or below the thread flushes (and, the first time, arranges to flush when
// it exits).
struct Thread {
  std::int64_t budget = 0;
  std::int64_t net = 0;
  std::uint64_t allocated = 0;
  std::uint64_t count = 0;
  Usage* target = nullptr;
  bool registered = false;
};
inline constexpr std::int64_t batch = 64 * 1024;

// The process's counts. Constant-initialized, so they are ready for the first allocation of the
// first static constructor.
struct Global {
  std::atomic<bool> installed{false};
  std::atomic<bool> counting{true};
  std::atomic<std::int64_t> current{0};
  std::atomic<std::int64_t> peak{0};
  std::atomic<std::uint64_t> allocated{0};
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> windows{0};  // a bit per open window
  std::array<std::atomic<std::int64_t>, 64> window_peak{};
};
constinit Global global{};

// The calling thread's record. `constinit` and trivially destructible, so reaching it is a plain
// thread-local access, never a guard.
constinit thread_local Thread thread_state{};

// Whether the replacements are linked into this program (`new.cpp` says so as it starts).
bool installed() noexcept;
void install() noexcept;
// Whether allocations are counted: on from the start; a process that never reads the counts (a
// plain in-process compile, a daemon's client) turns it off, and pays a load and a branch per
// allocation from then on.
bool counting() noexcept;
void disable() noexcept;
// What the allocation functions call (new.cpp).
void flush_slow(Thread& t) noexcept;

inline void on_allocate(std::size_t n) noexcept {
  if (!global.counting.load(std::memory_order_relaxed)) return;
  Thread& t = thread_state;
  const auto s = static_cast<std::int64_t>(n);
  t.net += s;
  t.allocated += n;
  ++t.count;
  if (Usage* u = t.target) u->add(n);
  if ((t.budget -= s) <= 0) flush_slow(t);
}
inline void on_free(std::size_t n) noexcept {
  if (!global.counting.load(std::memory_order_relaxed)) return;
  Thread& t = thread_state;
  const auto s = static_cast<std::int64_t>(n);
  t.net -= s;
  if (Usage* u = t.target) u->sub(n);
  if ((t.budget -= s) <= 0) flush_slow(t);
}

// The calling thread's target, replaced; returns the one it had.
inline Usage* exchange_target(Usage* u) noexcept { return std::exchange(thread_state.target, u); }
inline Usage* target() noexcept { return thread_state.target; }

// `u` as the target while it lives, then the one before; `nullptr` counts for nobody (engine
// objects one frame makes and another frees).
class Target {
 public:
  explicit Target(Usage* u) noexcept : before_(exchange_target(u)) {}
  Target(const Target&) = delete;
  Target& operator=(const Target&) = delete;
  ~Target() { exchange_target(before_); }

 private:
  Usage* before_;
};

// Adds the calling thread's counts to the process's now.
void flush() noexcept;
// The process's totals, after flushing the calling thread.
Totals process() noexcept;

// The process's high-water mark from now until it closes (a build's peak): every flush raises it
// to the process's current bytes. 64 can be open at once; a window opened past that reports the
// process's own peak.
class Window {
 public:
  Window() noexcept;
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;
  ~Window();
  std::int64_t peak() const noexcept;

 private:
  int slot_ = -1;
};

}  // namespace ycxx::memory

namespace ycxx::memory {

bool installed() noexcept { return global.installed.load(std::memory_order_relaxed); }
void install() noexcept { global.installed.store(true, std::memory_order_relaxed); }
bool counting() noexcept { return installed() && global.counting.load(std::memory_order_relaxed); }
void disable() noexcept { global.counting.store(false, std::memory_order_relaxed); }

void raise(std::atomic<std::int64_t>& to, std::int64_t v) noexcept {
  std::int64_t had = to.load(std::memory_order_relaxed);
  while (v > had && !to.compare_exchange_weak(had, v, std::memory_order_relaxed)) {
  }
}

void flush_thread(Thread& t) noexcept {
  if (t.net != 0) {
    const std::int64_t now = global.current.fetch_add(t.net, std::memory_order_relaxed) + t.net;
    raise(global.peak, now);
    for (std::uint64_t open = global.windows.load(std::memory_order_acquire); open != 0; open &= open - 1) {
      raise(global.window_peak[static_cast<std::size_t>(std::countr_zero(open))], now);
    }
  }
  if (t.allocated != 0) global.allocated.fetch_add(t.allocated, std::memory_order_relaxed);
  if (t.count != 0) global.count.fetch_add(t.count, std::memory_order_relaxed);
  t.net = 0;
  t.allocated = 0;
  t.count = 0;
  t.budget = batch;
}

// Flushes the thread's counts when it exits. Its thread-local has a destructor, so it is reached
// only once per thread, from the slow path.
struct AtExit {
  bool armed = false;
  ~AtExit() {
    if (armed) flush_thread(thread_state);
  }
};
thread_local AtExit at_exit;

void flush_slow(Thread& t) noexcept {
  if (!t.registered) {
    // First: registering the destructor may allocate, which comes back here.
    t.registered = true;
    at_exit.armed = true;
  }
  flush_thread(t);
}

void flush() noexcept { flush_thread(thread_state); }

Totals process() noexcept {
  flush();
  return {global.current.load(std::memory_order_relaxed), global.peak.load(std::memory_order_relaxed),
          global.allocated.load(std::memory_order_relaxed), global.count.load(std::memory_order_relaxed)};
}

Window::Window() noexcept {
  flush();
  std::uint64_t open = global.windows.load(std::memory_order_relaxed);
  while (open != ~std::uint64_t{0}) {
    const int s = std::countr_one(open);
    // The slot's start before it is open, so a flush never sees a stale peak.
    global.window_peak[static_cast<std::size_t>(s)].store(global.current.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (global.windows.compare_exchange_weak(open, open | (std::uint64_t{1} << s), std::memory_order_acq_rel)) {
      slot_ = s;
      return;
    }
  }
}

Window::~Window() {
  if (slot_ >= 0) global.windows.fetch_and(~(std::uint64_t{1} << slot_), std::memory_order_acq_rel);
}

std::int64_t Window::peak() const noexcept {
  flush();
  if (slot_ < 0) return global.peak.load(std::memory_order_relaxed);
  // At least what is allocated now: the last flush may predate the window's highest point.
  return std::max(global.window_peak[static_cast<std::size_t>(slot_)].load(std::memory_order_relaxed),
                  global.current.load(std::memory_order_relaxed));
}

}  // namespace ycxx::memory
