// Throwaway spike for yrashk/ycxx#1: a red/green query engine with early cutoff, ordered
// dependency groups, and fork-join parallel fetches on a small work-stealing pool.
// See docs/design/query-engine.md for the real design; this file only demonstrates it.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace qe {

using Revision = std::uint64_t;

// A query key: a kind plus two serialized arguments. Keys are totally ordered, and that
// order (never completion order) is the only order the engine exposes.
struct Key {
  std::uint16_t kind = 0;
  std::string a, b;
  friend auto operator<=>(const Key&, const Key&) = default;
  friend bool operator==(const Key&, const Key&) = default;
};

struct KeyHash {
  std::size_t operator()(const Key& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull ^ k.kind;
    auto mix = [&](const std::string& s) {
      for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
      h = (h ^ 0xff) * 1099511628211ull;
    };
    mix(k.a);
    mix(k.b);
    return static_cast<std::size_t>(h);
  }
};

// Values are immutable once published and compared by equality for early cutoff.
struct ValueBase {
  virtual ~ValueBase() = default;
  virtual bool equals(const ValueBase&) const = 0;
};

template <class T>
struct Boxed final : ValueBase {
  T v;
  explicit Boxed(T x) : v(std::move(x)) {}
  bool equals(const ValueBase& o) const override {
    auto* p = dynamic_cast<const Boxed*>(&o);
    return p != nullptr && p->v == v;
  }
};

using Value = std::shared_ptr<const ValueBase>;

template <class T>
Value box(T v) {
  return std::make_shared<const Boxed<T>>(std::move(v));
}

// The reference stays valid while the memo entry holding the value lives and is not replaced,
// which in this spike is the rest of the current revision: entries are never freed, an entry is
// re-executed at most once per revision, and inputs change only between requests. A real engine
// with garbage collection or cancellation must hand out owning handles instead.
template <class T>
const T& unbox(const Value& v) {
  return static_cast<const Boxed<T>&>(*v).v;
}

struct CycleError : std::runtime_error {
  explicit CycleError(const std::string& what) : std::runtime_error(what) {}
};

// ---------------------------------------------------------------------------------------------
// Work-stealing pool with fork-join. A joining worker only runs tasks of its *own* join (the top
// of its own deque); it never picks up unrelated work while a frame below it on its stack is
// blocked, which would turn ordinary dependencies into false deadlocks.

class Pool {
 public:
  explicit Pool(unsigned n) : deques_(std::max(n, 1u)) {
    n = size();
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this, i] { worker_loop(i); });
  }
  ~Pool() {
    stop_.store(true);
    {
      std::lock_guard lk(idle_m_);
      idle_cv_.notify_all();
    }
    for (auto& t : workers_) t.join();
  }
  unsigned size() const { return static_cast<unsigned>(deques_.size()); }

  // Runs all of fns[0..n) and returns when all are done, also when some throw: the lowest-index
  // exception is rethrown. So the set of functions run, and the failure reported, do not depend
  // on scheduling.
  void fork_join(std::span<const std::function<void()>> fns) {
    if (fns.empty()) return;
    const int self = self_pool_ == this ? self_ : -1;
    if (self < 0) return run_from_outside(fns);
    if (size() == 1 || fns.size() == 1) {
      std::exception_ptr first;
      for (auto& f : fns) {
        try {
          f();
        } catch (...) {
          if (!first) first = std::current_exception();
        }
      }
      if (first) std::rethrow_exception(first);
      return;
    }
    Join join;
    join.pending.store(static_cast<int>(fns.size()) - 1);
    std::vector<Task> tasks(fns.size());
    std::vector<std::exception_ptr> errors(fns.size());
    for (std::size_t i = 0; i < fns.size(); ++i)
      tasks[i] = Task{[&, i] {
                        try {
                          fns[i]();
                        } catch (...) {
                          errors[i] = std::current_exception();
                        }
                      },
                      &join};
    {
      auto& d = deques_[self];
      std::lock_guard lk(d.m);
      // Push in reverse so the owner pops them in index order (LIFO); thieves take from the front.
      for (std::size_t i = fns.size(); i-- > 1;) d.q.push_back(&tasks[i]);
    }
    wake();
    tasks[0].fn();
    while (join.pending.load() > 0) {
      Task* t = nullptr;
      {
        auto& d = deques_[self];
        std::lock_guard lk(d.m);
        if (!d.q.empty() && d.q.back()->join == &join) {
          t = d.q.back();
          d.q.pop_back();
        }
      }
      if (t != nullptr) {
        run(t);
        continue;
      }
      std::unique_lock lk(join.m);
      join.cv.wait(lk, [&] { return join.pending.load() == 0; });
    }
    // The last finisher decrements under join.m; taking it here waits until that thread has
    // let go of `join`, which is about to be destroyed.
    { std::lock_guard lk(join.m); }
    for (auto& e : errors)
      if (e) std::rethrow_exception(e);
  }

 private:
  struct Join {
    std::atomic<int> pending{0};
    std::mutex m;
    std::condition_variable cv;
  };
  struct Task {
    std::function<void()> fn;
    Join* join = nullptr;
  };
  struct Deque {
    std::mutex m;
    std::deque<Task*> q;
  };

  static void run(Task* t) {
    Join* j = t->join;  // `t` lives in the joiner's frame: do not touch it once pending drops
    t->fn();
    std::lock_guard lk(j->m);
    if (j->pending.fetch_sub(1) == 1) j->cv.notify_all();
  }

  void run_from_outside(std::span<const std::function<void()>> fns) {
    // An external thread hands the whole batch to worker 0 as one task and waits.
    Join join;
    join.pending.store(1);
    std::exception_ptr err;
    Task t{[&] {
             try {
               fork_join(fns);
             } catch (...) {
               err = std::current_exception();
             }
           },
           &join};
    {
      std::lock_guard lk(inject_m_);
      inject_.push_back(&t);
    }
    wake();
    {
      std::unique_lock lk(join.m);
      join.cv.wait(lk, [&] { return join.pending.load() == 0; });
    }
    if (err) std::rethrow_exception(err);
  }

  Task* steal(unsigned self) {
    {
      std::lock_guard lk(inject_m_);
      if (!inject_.empty()) {
        Task* t = inject_.front();
        inject_.pop_front();
        return t;
      }
    }
    for (unsigned k = 1; k <= size(); ++k) {
      auto& d = deques_[(self + k) % size()];
      std::lock_guard lk(d.m);
      if (!d.q.empty()) {
        Task* t = d.q.front();
        d.q.pop_front();
        return t;
      }
    }
    return nullptr;
  }

  void wake() {
    std::lock_guard lk(idle_m_);
    idle_cv_.notify_all();
  }

  void worker_loop(unsigned i) {
    self_pool_ = this;
    self_ = static_cast<int>(i);
    while (!stop_.load()) {
      if (Task* t = steal(i)) {
        run(t);
        continue;
      }
      std::unique_lock lk(idle_m_);
      idle_cv_.wait_for(lk, std::chrono::microseconds(200));
    }
  }

  // Which pool, if any, the current thread is a worker of: a worker of another pool is an
  // outside caller here.
  static inline thread_local const Pool* self_pool_ = nullptr;
  static inline thread_local int self_ = -1;
  std::vector<Deque> deques_;
  std::mutex inject_m_;
  std::deque<Task*> inject_;
  std::mutex idle_m_;
  std::condition_variable idle_cv_;
  std::atomic<bool> stop_{false};
  std::vector<std::thread> workers_;
};

// ---------------------------------------------------------------------------------------------

class Engine;

// One active execution or validation of a query. Frames link to the frame that requested them,
// across threads, so the requesting chain can be walked for cycle detection.
struct Frame {
  const Key* key = nullptr;
  const Frame* parent = nullptr;
  bool record = false;
  std::vector<std::vector<Key>> deps;  // ordered groups, in the query's program order
};

class Ctx {
 public:
  Ctx(Engine& e, Frame& f) : engine_(e), frame_(f) {}
  Value fetch(const Key& k);
  // Parallel fetch. Recorded as one dependency group in argument order; results are returned in
  // argument order, whatever order they complete in.
  std::vector<Value> fetch_all(std::span<const Key> ks);
  // Deliberately broken read that bypasses dependency recording (fault injection for the
  // incremental == cold checker; never used by correct queries).
  Value fetch_untracked(const Key& k);

 private:
  Engine& engine_;
  Frame& frame_;
};

using QueryFn = std::function<Value(Ctx&, const Key&)>;

struct LogEntry {
  Key key;
  bool changed;  // false: re-executed but produced an equal value (early cutoff)
  friend auto operator<=>(const LogEntry&, const LogEntry&) = default;
};

class Engine {
 public:
  explicit Engine(unsigned threads) : pool_(threads) {}

  void define(std::uint16_t kind, QueryFn fn) {
    if (fns_.size() <= kind) fns_.resize(kind + 1);
    fns_[kind] = std::move(fn);
  }

  // Inputs may only be set between requests (the spike has no cancellation).
  void set_input(const Key& k, Value v) {
    Entry& e = entry(k);
    std::lock_guard lk(e.m);
    ++rev_;
    e.is_input = true;
    e.state = Entry::State::Done;
    if (!e.value || !e.value->equals(*v)) {
      e.value = std::move(v);
      e.changed_at = rev_;
    }
    e.verified_at = rev_;
  }

  Value get(const Key& k) {
    Value out;
    std::function<void()> f = [&] { out = fetch(nullptr, k, nullptr).first; };
    pool_.fork_join(std::span(&f, 1));
    return out;
  }

  // Queries executed since the last call, sorted by key.
  std::vector<LogEntry> take_log() {
    std::lock_guard lk(log_m_);
    auto out = std::move(log_);
    log_.clear();
    std::sort(out.begin(), out.end());
    return out;
  }

  // A canonical dump of the whole memo graph, for comparing runs at different thread counts.
  std::string graph_digest() {
    std::vector<std::pair<Key, std::string>> rows;
    for (auto& s : shards_) {
      std::lock_guard lk(s.m);
      for (auto& [k, e] : s.map) {
        std::string r = std::to_string(e->changed_at) + "/" + std::to_string(e->verified_at) +
                        "/x" + std::to_string(e->executions) + " deps:";
        for (auto& g : e->deps) {
          r += "[";
          for (auto& d : g) r += std::to_string(d.kind) + ":" + d.a + ":" + d.b + ";";
          r += "]";
        }
        rows.emplace_back(k, std::move(r));
      }
    }
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (auto& [k, r] : rows) out += std::to_string(k.kind) + ":" + k.a + ":" + k.b + " " + r + "\n";
    return out;
  }

  Revision revision() const { return rev_; }

 private:
  friend class Ctx;

  struct Entry {
    enum class State : std::uint8_t { Empty, Running, Done };
    std::mutex m;
    std::condition_variable cv;
    State state = State::Empty;
    bool is_input = false;
    Value value;
    Revision changed_at = 0, verified_at = 0;
    std::vector<std::vector<Key>> deps;
    int executions = 0;
  };

  struct Shard {
    std::mutex m;
    std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> map;
  };

  Entry& entry(const Key& k) {
    Shard& s = shards_[KeyHash{}(k) % shards_.size()];
    std::lock_guard lk(s.m);
    auto& p = s.map[k];
    if (!p) p = std::make_unique<Entry>();
    return *p;  // entries are never freed while the engine lives, so the reference stays valid
  }

  static bool on_chain(const Frame* f, const Key& k) {
    for (; f != nullptr; f = f->parent)
      if (*f->key == k) return true;
    return false;
  }

  // Brings k up to date for the current revision; returns its value and changed_at.
  std::pair<Value, Revision> fetch(const Frame* requester, const Key& k, Frame* recorder) {
    if (recorder != nullptr && recorder->record) recorder->deps.push_back({k});
    return fetch_unrecorded(requester, k);
  }

  std::pair<Value, Revision> fetch_unrecorded(const Frame* requester, const Key& k) {
    Entry& e = entry(k);
    std::unique_lock lk(e.m);
    for (;;) {
      if (e.is_input) return {e.value, e.changed_at};
      if (e.state == Entry::State::Done && e.verified_at == rev_) return {e.value, e.changed_at};
      if (e.state == Entry::State::Running) {
        if (on_chain(requester, k)) throw CycleError("query cycle through kind " + std::to_string(k.kind) + " " + k.a + " " + k.b);
        e.cv.wait(lk);
        continue;
      }
      break;
    }
    const bool had_value = e.state == Entry::State::Done;
    const auto prev = e.state;
    e.state = Entry::State::Running;  // claimed: this thread alone brings e up to date
    lk.unlock();

    Frame f{&k, requester, false, {}};
    try {
      if (!(had_value && deep_verify(e, f))) execute(e, k, f);
    } catch (...) {
      lk.lock();
      e.state = prev;
      e.cv.notify_all();
      throw;
    }
    lk.lock();
    e.verified_at = rev_;
    e.state = Entry::State::Done;
    e.cv.notify_all();
    return {e.value, e.changed_at};
  }

  // Green check: every recorded dependency, group by group in recorded order, is brought up to
  // date; any that changed after we were last verified forces re-execution. Members of a group
  // were fetched together, so they are validated together, in parallel.
  bool deep_verify(Entry& e, Frame& f) {
    for (const auto& group : e.deps) {
      std::vector<Revision> changed(group.size());
      fetch_group(f, group, nullptr, &changed);
      for (Revision c : changed)
        if (c > e.verified_at) return false;
    }
    return true;
  }

  void execute(Entry& e, const Key& k, Frame& f) {
    f.record = true;
    f.deps.clear();
    Ctx ctx(*this, f);
    Value v = fns_.at(k.kind)(ctx, k);
    const bool changed = !e.value || !e.value->equals(*v);
    if (changed) {
      e.value = std::move(v);
      e.changed_at = rev_;
    }
    e.deps = std::move(f.deps);
    ++e.executions;
    std::lock_guard lk(log_m_);
    log_.push_back({k, changed});
  }

  void fetch_group(Frame& f, std::span<const Key> ks, std::vector<Value>* values,
                   std::vector<Revision>* changed) {
    if (values) values->assign(ks.size(), nullptr);
    std::vector<std::function<void()>> fns;
    fns.reserve(ks.size());
    for (std::size_t i = 0; i < ks.size(); ++i)
      fns.push_back([&, i] {
        auto [v, c] = fetch_unrecorded(&f, ks[i]);
        if (values) (*values)[i] = std::move(v);
        if (changed) (*changed)[i] = c;
      });
    pool_.fork_join(fns);
  }

  Pool pool_;
  std::vector<QueryFn> fns_;
  std::array<Shard, 64> shards_;
  Revision rev_ = 1;
  std::mutex log_m_;
  std::vector<LogEntry> log_;
};

inline Value Ctx::fetch(const Key& k) { return engine_.fetch(&frame_, k, &frame_).first; }

inline std::vector<Value> Ctx::fetch_all(std::span<const Key> ks) {
  if (frame_.record) frame_.deps.emplace_back(ks.begin(), ks.end());
  std::vector<Value> out;
  engine_.fetch_group(frame_, ks, &out, nullptr);
  return out;
}

inline Value Ctx::fetch_untracked(const Key& k) { return engine_.fetch_unrecorded(&frame_, k).first; }

}  // namespace qe
