// The work-stealing pool (docs/design/query-engine.md §A3.2, §A3.5, #15): P workers, each with a
// Chase–Lev deque per priority; idle workers steal the oldest task of a random victim, trying
// every `interactive` deque before any `background` one, then park. Threads that are not
// workers push onto an injection queue. Every resumption starts at a worker loop's depth, so
// stacks never nest.
//
// The pool is a `std::execution` scheduler (`pool_scheduler`), and `pool_backend` implements
// `parallel_scheduler_replacement::parallel_scheduler_backend` over it, so a program that
// installs it (the daemon, #83) makes `std::execution::parallel_scheduler` this pool.
//
// The chaos scheduler (§A5.5, §A9) is the same pool with a seed: it reverses deques, steals
// before popping, migrates tasks through the injection queue and adds short delays at fetch
// misses. Its decisions come from the seed and a per-worker counter, never from time, but which
// thread reaches a decision first still depends on the machine, so a seed reproduces a
// distribution of schedules, not one schedule (#19 keeps the open item).
export module ycxx.engine:pool;

import std;

export namespace ycxx::engine {

// A root request's priority (§A3.5). Continuations run at their root's.
enum class Priority : std::uint8_t { interactive, background };
inline constexpr std::size_t priority_levels = 2;

// Intrusive work: an operation state or an engine record that runs itself.
struct Task {
  void (*run)(Task*) noexcept = nullptr;
};

// A unit of work: a coroutine to resume, or a task to run. One word, tagged in its low bit.
class Work {
public:
  Work() = default;
  Work(std::coroutine_handle<> h) noexcept : bits_(reinterpret_cast<std::uintptr_t>(h.address())) {}
  Work(Task* t) noexcept : bits_(reinterpret_cast<std::uintptr_t>(t) | 1) {}
  explicit operator bool() const noexcept { return bits_ != 0; }
  void operator()() const noexcept {
    if (bits_ & 1) {
      Task* t = reinterpret_cast<Task*>(bits_ & ~std::uintptr_t{1});
      t->run(t);
    } else {
      std::coroutine_handle<>::from_address(reinterpret_cast<void*>(bits_)).resume();
    }
  }
  std::uintptr_t bits() const noexcept { return bits_; }
  static Work from_bits(std::uintptr_t b) noexcept {
    Work w;
    w.bits_ = b;
    return w;
  }

private:
  std::uintptr_t bits_ = 0;
};

class pool_scheduler;

class Pool {
public:
  // `threads` workers; with a seed, the chaos scheduler.
  explicit Pool(unsigned threads, std::optional<std::uint64_t> chaos_seed = std::nullopt);
  // A worker's stack: queries recurse as deep as on a main thread, whose stack is 8 MiB by
  // default, and far deeper is reserved, as address space only (#178; a guard is #187).
  static constexpr std::size_t worker_stack = std::size_t{256} << 20;
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  // Precondition: nothing is queued or running.
  ~Pool();

  unsigned size() const noexcept { return static_cast<unsigned>(workers_.size()); }
  bool chaotic() const noexcept { return chaos_.has_value(); }

  // Queues work: on the calling worker's own deque, or on the injection queue from any other
  // thread. Never runs it inline.
  void push(Work w, Priority p) noexcept;

  // The calling thread's worker index in this pool, if it is one of its workers.
  std::optional<unsigned> worker_index() const noexcept;

  // Under the chaos scheduler, now and then a short random delay on a worker. Changes no value.
  void chaos_point() noexcept;

  pool_scheduler scheduler(Priority p = Priority::background) noexcept;

  // What query code reads besides its keys, entered on a worker before it runs work (#137): the
  // pipeline's universe and AST interners, bound on thread-locals. A worker calls `enter` once per
  // scope it sees. Every work item runs entered, the `pool_scheduler`'s and the
  // `parallel_scheduler` backend's too, which is harmless. The thread that starts a request runs
  // query code as well (`Request::begin`, cheap kinds), and must be bound by its owner.
  // `exit` undoes `enter` on a worker when the scope is released or replaced; it is copied when
  // entered, so it runs even after the scope's owner is gone.
  struct Scope {
    void* ctx = nullptr;
    void (*enter)(void*) noexcept = nullptr;
    void (*exit)() noexcept = nullptr;
  };
  // One scope at a time: a pool serves one session. Claims the pool for `s`, which must be free.
  // Precondition: nothing is queued or running.
  void claim_scope(const Scope* s) noexcept {
    const Scope* none = nullptr;
    const bool free = scope_.compare_exchange_strong(none, s, std::memory_order_acq_rel);
    contract_assert(free);  // another session holds this pool
  }
  // Releases the pool if `s` holds it. Precondition: nothing is queued or running.
  void release_scope(const Scope* s) noexcept {
    const Scope* held = s;
    scope_.compare_exchange_strong(held, nullptr, std::memory_order_acq_rel);
  }

private:
  class Deque;
  struct Worker;

  void loop(unsigned index) noexcept;
  Work find(Worker& w) noexcept;
  Work steal(Worker& self, std::size_t prio) noexcept;
  Work pop_injected(std::size_t prio) noexcept;
  bool has_work() const noexcept;
  void park() noexcept;
  void wake() noexcept;

  std::vector<std::unique_ptr<Worker>> workers_;
  struct Injection {
    std::mutex m;
    std::deque<Work> q;
    std::atomic<std::size_t> size{0};
  };
  std::array<Injection, priority_levels> injected_;
  std::atomic<std::uint32_t> epoch_{0};
  std::atomic<std::uint32_t> sleepers_{0};
  std::atomic<bool> stopping_{false};
  std::optional<std::uint64_t> chaos_;
  std::atomic<const Scope*> scope_{nullptr};
};

// `std::execution::scheduler` over the pool: `schedule()` completes on a worker, at the
// scheduler's priority (§A3.1).
class pool_scheduler {
public:
  using scheduler_concept = std::execution::scheduler_tag;

  pool_scheduler(Pool& p, Priority prio) noexcept : pool_(&p), prio_(prio) {}

  template <class R>
  struct op : Task {
    using operation_state_concept = std::execution::operation_state_tag;
    op(Pool* p, Priority prio, R r) : rcvr(std::move(r)), pool(p), prio(prio) { run = &op::go; }
    op(const op&) = delete;
    void start() & noexcept { pool->push(this, prio); }

    R rcvr;
    Pool* pool;
    Priority prio;

  private:
    static void go(Task* t) noexcept {
      auto& o = *static_cast<op*>(t);
      if constexpr (std::unstoppable_token<std::stop_token_of_t<std::execution::env_of_t<R>>>) {
        std::execution::set_value(std::move(o.rcvr));
      } else if (std::get_stop_token(std::execution::get_env(o.rcvr)).stop_requested()) {
        std::execution::set_stopped(std::move(o.rcvr));
      } else {
        std::execution::set_value(std::move(o.rcvr));
      }
    }
  };

  class sender {
  public:
    using sender_concept = std::execution::sender_tag;
    template <class Self, class... Env>
    static consteval auto get_completion_signatures() {
      if constexpr (sizeof...(Env) != 0 && (std::unstoppable_token<std::stop_token_of_t<Env>> && ...)) {
        return std::execution::completion_signatures<std::execution::set_value_t()>();
      } else {
        return std::execution::completion_signatures<std::execution::set_value_t(), std::execution::set_stopped_t()>();
      }
    }
    struct attrs {
      Pool* pool;
      Priority prio;
      template <class Tag>
        requires (std::is_same_v<Tag, std::execution::set_value_t> || std::is_same_v<Tag, std::execution::set_stopped_t>)
      pool_scheduler query(std::execution::get_completion_scheduler_t<Tag>) const noexcept {
        return {*pool, prio};
      }
    };
    attrs get_env() const noexcept { return {pool_, prio_}; }
    template <class R>
    op<std::decay_t<R>> connect(R&& r) const {
      return {pool_, prio_, std::forward<R>(r)};
    }

  private:
    friend class pool_scheduler;
    sender(Pool* p, Priority prio) noexcept : pool_(p), prio_(prio) {}
    Pool* pool_;
    Priority prio_;
  };

  sender schedule() const noexcept { return {pool_, prio_}; }
  bool operator==(const pool_scheduler&) const noexcept = default;
  std::execution::forward_progress_guarantee query(std::execution::get_forward_progress_guarantee_t) const noexcept {
    return std::execution::forward_progress_guarantee::parallel;
  }

private:
  Pool* pool_;
  Priority prio_;
};

// The pool as `std::execution::parallel_scheduler`'s backend (§A3.1). A program installs it by
// defining `parallel_scheduler_replacement::query_parallel_scheduler_backend` to return it.
std::shared_ptr<std::execution::parallel_scheduler_replacement::parallel_scheduler_backend> pool_backend(Pool& pool);

}  // namespace ycxx::engine

namespace ycxx::engine {

namespace {

constexpr std::uint64_t splitmix(std::uint64_t& s) noexcept {
  std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// The calling thread's worker, if it is one. Engine-internal: query code never reads it, and
// the engine reads it afresh after every suspension (§A3.7).
struct Current {
  const Pool* pool = nullptr;
  unsigned index = 0;
};
thread_local Current current;

}  // namespace

// A Chase–Lev deque of `Work` words (Lê, Pop, Cohen and Zappa Nardelli's formulation for weak
// memory). The owner pushes and takes at the bottom; thieves take the oldest at the top.
class Pool::Deque {
public:
  Deque() : array_(new Array(64)) { retired_.emplace_back(array_.load(std::memory_order_relaxed)); }

  void push(Work w) noexcept {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_acquire);
    Array* a = array_.load(std::memory_order_relaxed);
    if (b - t > a->mask) a = grow(a, t, b);
    a->at(b).store(w.bits(), std::memory_order_relaxed);
    // A release store, where the formulation has a release fence and a relaxed store: the same
    // order (a thief's acquire load of `bottom_` sees the slot and the frame it names), and one
    // ThreadSanitizer can see, since it does not model fences (#105).
    bottom_.store(b + 1, std::memory_order_release);
  }

  // The newest work, by the owner.
  Work take() noexcept {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Array* a = array_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t t = top_.load(std::memory_order_relaxed);
    if (t > b) {
      bottom_.store(b + 1, std::memory_order_relaxed);
      return {};
    }
    std::uintptr_t x = a->at(b).load(std::memory_order_relaxed);
    if (t == b) {
      if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) x = 0;
      bottom_.store(b + 1, std::memory_order_relaxed);
    }
    return Work::from_bits(x);
  }

  // The oldest work, by any thread. Empty also when it loses a race; callers move on.
  Work steal() noexcept {
    std::int64_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::int64_t b = bottom_.load(std::memory_order_acquire);
    if (t >= b) return {};
    Array* a = array_.load(std::memory_order_acquire);
    const std::uintptr_t x = a->at(t).load(std::memory_order_relaxed);
    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) return {};
    return Work::from_bits(x);
  }

  bool empty() const noexcept {
    return bottom_.load(std::memory_order_relaxed) <= top_.load(std::memory_order_relaxed);
  }

private:
  struct Array {
    explicit Array(std::int64_t n) : mask(n - 1), slots(new std::atomic<std::uintptr_t>[static_cast<std::size_t>(n)]) {}
    std::atomic<std::uintptr_t>& at(std::int64_t i) noexcept { return slots[static_cast<std::size_t>(i & mask)]; }
    std::int64_t mask;
    std::unique_ptr<std::atomic<std::uintptr_t>[]> slots;
  };
  // Thieves may still read the old array, so it is kept until the pool goes away.
  Array* grow(Array* a, std::int64_t t, std::int64_t b) {
    auto bigger = std::make_unique<Array>(2 * (a->mask + 1));
    for (std::int64_t i = t; i < b; ++i) bigger->at(i).store(a->at(i).load(std::memory_order_relaxed), std::memory_order_relaxed);
    Array* n = bigger.get();
    retired_.push_back(std::move(bigger));
    array_.store(n, std::memory_order_release);
    return n;
  }

  alignas(64) std::atomic<std::int64_t> top_{0};
  alignas(64) std::atomic<std::int64_t> bottom_{0};
  std::atomic<Array*> array_;
  std::vector<std::unique_ptr<Array>> retired_;
};

struct Pool::Worker {
  std::array<Deque, priority_levels> deques;
  std::uint64_t rng = 0;
  std::jthread thread;
};

Pool::Pool(unsigned threads, std::optional<std::uint64_t> chaos_seed) : chaos_(chaos_seed) {
  contract_assert(threads > 0);
  for (unsigned i = 0; i < threads; ++i) {
    auto w = std::make_unique<Worker>();
    w->rng = chaos_seed.value_or(0x79637878) ^ (0x9e3779b97f4a7c15ull * (i + 1));
    workers_.push_back(std::move(w));
  }
  for (unsigned i = 0; i < threads; ++i)
    workers_[i]->thread = std::jthread(std::jthread::stack_size_hint(worker_stack), [this, i] { loop(i); });
}

Pool::~Pool() {
  stopping_.store(true, std::memory_order_seq_cst);
  epoch_.fetch_add(1, std::memory_order_seq_cst);
  epoch_.notify_all();
  for (auto& w : workers_) w->thread.join();
}

std::optional<unsigned> Pool::worker_index() const noexcept {
  if (current.pool != this) return std::nullopt;
  return current.index;
}

void Pool::push(Work w, Priority p) noexcept {
  const auto prio = static_cast<std::size_t>(std::to_underlying(p));
  if (current.pool == this) {
    workers_[current.index]->deques[prio].push(w);
  } else {
    Injection& in = injected_[prio];
    std::lock_guard lock(in.m);
    in.q.push_back(w);
    in.size.fetch_add(1, std::memory_order_relaxed);
  }
  wake();
}

// A pusher that sees no sleeper is ordered before the sleeper's own check for work (both sides
// put a sequentially consistent fence between their store and their load), so no push is lost.
void Pool::wake() noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (sleepers_.load(std::memory_order_relaxed) != 0) {
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    epoch_.notify_one();
  }
}

void Pool::park() noexcept {
  sleepers_.fetch_add(1, std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const std::uint32_t e = epoch_.load(std::memory_order_seq_cst);
  if (!stopping_.load(std::memory_order_seq_cst) && !has_work()) epoch_.wait(e, std::memory_order_seq_cst);
  sleepers_.fetch_sub(1, std::memory_order_seq_cst);
}

bool Pool::has_work() const noexcept {
  for (const auto& in : injected_) {
    if (in.size.load(std::memory_order_relaxed) != 0) return true;
  }
  for (const auto& w : workers_) {
    for (const auto& d : w->deques) {
      if (!d.empty()) return true;
    }
  }
  return false;
}

Work Pool::pop_injected(std::size_t prio) noexcept {
  Injection& in = injected_[prio];
  if (in.size.load(std::memory_order_relaxed) == 0) return {};
  std::lock_guard lock(in.m);
  if (in.q.empty()) return {};
  const Work w = in.q.front();
  in.q.pop_front();
  in.size.fetch_sub(1, std::memory_order_relaxed);
  return w;
}

Work Pool::steal(Worker& self, std::size_t prio) noexcept {
  const std::size_t n = workers_.size();
  const std::size_t start = static_cast<std::size_t>(splitmix(self.rng) % n);
  for (std::size_t k = 0; k < n; ++k) {
    Worker& victim = *workers_[(start + k) % n];
    if (&victim == &self) continue;
    if (const Work w = victim.deques[prio].steal()) return w;
  }
  return {};
}

// Interactive work anywhere before background work anywhere (§A3.5).
Work Pool::find(Worker& w) noexcept {
  for (std::size_t prio = 0; prio < priority_levels; ++prio) {
    if (chaos_) {
      const std::uint64_t r = splitmix(w.rng);
      if (r % 4 == 0) {
        if (const Work x = w.deques[prio].steal()) return x;  // the deque reversed
      } else if (r % 4 == 1) {
        if (const Work x = steal(w, prio)) return x;  // steal before popping
      }
    }
    if (const Work x = w.deques[prio].take()) return x;
    if (const Work x = steal(w, prio)) return x;
    if (const Work x = pop_injected(prio)) return x;
  }
  return {};
}

void Pool::loop(unsigned index) noexcept {
  current = {this, index};
  Worker& self = *workers_[index];
  const Scope* entered = nullptr;
  void (*exit_entered)() noexcept = nullptr;
  unsigned idle = 0;
  while (true) {
    if (const Work w = find(self)) {
      idle = 0;
      if (chaos_ && splitmix(self.rng) % 8 == 0) {
        // Migrate: whoever finds it next runs it, often another worker.
        Injection& in = injected_[1];
        {
          std::lock_guard lock(in.m);
          in.q.push_back(w);
          in.size.fetch_add(1, std::memory_order_relaxed);
        }
        wake();
        continue;
      }
      if (const Scope* sc = scope_.load(std::memory_order_acquire); sc != entered) {
        if (exit_entered) exit_entered();
        exit_entered = nullptr;
        if (sc) {
          sc->enter(sc->ctx);
          exit_entered = sc->exit;
        }
        entered = sc;
      }
      w();
      continue;
    }
    if (stopping_.load(std::memory_order_acquire)) break;
    if (++idle < 64) {
      std::this_thread::yield();
      continue;
    }
    park();
    idle = 0;
  }
  current = {};
}

void Pool::chaos_point() noexcept {
  if (!chaos_ || current.pool != this) return;
  Worker& self = *workers_[current.index];
  const std::uint64_t r = splitmix(self.rng);
  if (r % 16 != 0) return;
  for (std::uint64_t spins = (r >> 8) % 512; spins > 0; --spins) {
    if (spins % 64 == 0) std::this_thread::yield();
  }
}

pool_scheduler Pool::scheduler(Priority p) noexcept { return {*this, p}; }

namespace {

namespace psr = std::execution::parallel_scheduler_replacement;

class Backend final : public psr::parallel_scheduler_backend {
public:
  explicit Backend(Pool& p) : pool_(&p) {}

  void schedule(psr::receiver_proxy& r, std::span<std::byte> storage) noexcept override {
    struct One : Task {
      psr::receiver_proxy* r;
      bool heap;
    };
    One* t = storage.size() >= sizeof(One) ? std::construct_at(reinterpret_cast<One*>(storage.data())) : new One;
    t->heap = storage.size() < sizeof(One);
    t->r = &r;
    t->run = [](Task* task) noexcept {
      One* o = static_cast<One*>(task);
      psr::receiver_proxy& rcvr = *o->r;
      if (o->heap) delete o;
      const auto tok = rcvr.try_query<std::inplace_stop_token>(std::get_stop_token);
      if (tok && tok->stop_requested()) rcvr.set_stopped();
      else rcvr.set_value();
    };
    pool_->push(t, Priority::background);
  }

  void schedule_bulk_chunked(std::size_t n, psr::bulk_item_receiver_proxy& r, std::span<std::byte>) noexcept override {
    bulk(n, r, std::max<std::size_t>(1, (n + pool_->size() - 1) / pool_->size()));
  }
  void schedule_bulk_unchunked(std::size_t n, psr::bulk_item_receiver_proxy& r, std::span<std::byte>) noexcept override {
    bulk(n, r, 1);
  }

private:
  // `n` iterations in chunks of `chunk`, one task each; the last to finish completes.
  void bulk(std::size_t n, psr::bulk_item_receiver_proxy& r, std::size_t chunk) {
    if (n == 0) {
      r.set_value();
      return;
    }
    struct Chunk : Task {
      std::size_t begin = 0, end = 0;
      struct Shared* shared = nullptr;
    };
    struct Shared {
      psr::bulk_item_receiver_proxy* r;
      std::atomic<std::size_t> left;
      std::vector<Chunk> chunks;
    };
    const std::size_t count = (n + chunk - 1) / chunk;
    auto* s = new Shared{&r, count, std::vector<Chunk>(count)};
    for (std::size_t i = 0; i < count; ++i) {
      Chunk& c = s->chunks[i];
      c.begin = i * chunk;
      c.end = std::min(n, c.begin + chunk);
      c.shared = s;
      c.run = [](Task* task) noexcept {
        Chunk& me = *static_cast<Chunk*>(task);
        Shared* sh = me.shared;
        const auto tok = sh->r->try_query<std::inplace_stop_token>(std::get_stop_token);
        if (!tok || !tok->stop_requested()) sh->r->execute(me.begin, me.end);
        if (sh->left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          psr::bulk_item_receiver_proxy& rcvr = *sh->r;
          const bool stopped = tok && tok->stop_requested();
          delete sh;
          if (stopped) rcvr.set_stopped();
          else rcvr.set_value();
        }
      };
    }
    // The last chunk may finish and free `s` before this loop ends, so it walks a copy.
    std::vector<Task*> tasks;
    for (Chunk& c : s->chunks) tasks.push_back(&c);
    for (Task* t : tasks) pool_->push(t, Priority::background);
  }

  Pool* pool_;
};

}  // namespace

std::shared_ptr<psr::parallel_scheduler_backend> pool_backend(Pool& pool) { return std::make_shared<Backend>(pool); }

}  // namespace ycxx::engine
