// Storage for the interners that several workers intern into at once (#107; docs/design/ast.md
// §9.1, §9.4; query-engine.md §A3.6). Pools are append-only and never move, so a reader needs no
// lock: it learnt its handle through something that synchronized (the index's shard lock, or an
// engine cell published with release), and the bytes behind the handle were written before.
// Writers serialize per fingerprint on one shard of the index, so equal content still interns to
// one handle; which worker interned first decides only the handle's number, never a fingerprint
// (§2, invariant 3).
export module ycxx.ast.store;

import std;
import ycxx.ast.fingerprint;
import ycxx.ast.handles;

export namespace ycxx::ast {

// Append-only units of `unit` bytes in blocks that never move: block b holds `1024 << b` units.
// `alloc(n)` hands out n contiguous units that never cross a block (the rest of a block, or whole
// blocks too small for the range, are skipped instead), so a range is one span; with `alloc(1)`
// only, indices are dense. Moves only at quiescence (compaction).
//
// Reads use relaxed loads: whoever reads unit i learnt i through something that synchronized with
// its writer (an index's shard lock, or an engine cell published with release), and the writer
// allocated the block and wrote the unit before that.
class Chunked {
public:
  explicit Chunked(u32 unit = 1) : unit_(unit) {}
  Chunked(const Chunked&) = delete;
  Chunked& operator=(const Chunked&) = delete;
  Chunked(Chunked&& o) noexcept : unit_(o.unit_), cursor_(std::exchange(o.cursor_, 0)) {
    for (std::size_t b = 0; b < blocks; ++b) blocks_[b].store(o.blocks_[b].exchange(nullptr, std::memory_order_relaxed), std::memory_order_relaxed);
    size_.store(o.size_.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
  }
  Chunked& operator=(Chunked&& o) noexcept {
    if (this != &o) {
      release();
      unit_ = o.unit_;
      cursor_ = std::exchange(o.cursor_, 0);
      for (std::size_t b = 0; b < blocks; ++b) blocks_[b].store(o.blocks_[b].exchange(nullptr, std::memory_order_relaxed), std::memory_order_relaxed);
      size_.store(o.size_.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
  }
  ~Chunked() { release(); }

  u32 unit() const noexcept { return unit_; }
  // A high-water mark: units handed out, with skipped ones (a block's tail, whole blocks), which
  // are not readable. Dense only for a store filled with `alloc(1)`.
  u32 size() const noexcept { return size_.load(std::memory_order_relaxed); }

  u32 alloc(u32 n) {
    std::lock_guard lock(m_);
    u32 at = cursor_;
    if (n > 0) {
      auto [b, off] = locate(at);
      while (off + n > block_units(b)) {  // skip to the first block with room for the whole range
        ++b;
        off = 0;
        contract_assert(b < blocks);
      }
      at = static_cast<u32>(block_base(b) + off);
      block(b);
    }
    cursor_ = at + n;
    size_.store(cursor_, std::memory_order_release);
    return at;
  }

  // Makes unit `i` writable, for columns indexed by another pool's handle.
  void ensure(u32 i) { block(locate(i).first); }

  std::byte* at(u32 i) const noexcept {
    const auto [b, off] = locate(i);
    return blocks_[b].load(std::memory_order_relaxed) + std::size_t(off) * unit_;
  }

private:
  static constexpr unsigned first_bits = 10;
  static constexpr std::size_t blocks = 22;  // 2^32 units in all
  static constexpr std::size_t block_units(std::size_t b) noexcept { return std::size_t{1} << (first_bits + b); }
  static constexpr std::size_t block_base(std::size_t b) noexcept { return block_units(b) - block_units(0); }
  static constexpr std::pair<std::size_t, std::size_t> locate(u32 i) noexcept {
    const std::uint64_t biased = std::uint64_t{i} + block_units(0);
    const std::size_t b = static_cast<std::size_t>(std::bit_width(biased)) - 1 - first_bits;
    return {b, static_cast<std::size_t>(biased - block_units(b))};
  }
  std::byte* block(std::size_t b) {
    contract_assert(b < blocks);
    std::byte* p = blocks_[b].load(std::memory_order_acquire);
    if (p) return p;
    auto* fresh = static_cast<std::byte*>(::operator new(block_units(b) * unit_, std::align_val_t{alignof(std::max_align_t)}));
    if (blocks_[b].compare_exchange_strong(p, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) return fresh;
    ::operator delete(fresh, block_units(b) * unit_, std::align_val_t{alignof(std::max_align_t)});
    return p;
  }
  void release() noexcept {
    for (std::size_t b = 0; b < blocks; ++b) {
      if (std::byte* p = blocks_[b].exchange(nullptr, std::memory_order_relaxed)) {
        ::operator delete(p, block_units(b) * unit_, std::align_val_t{alignof(std::max_align_t)});
      }
    }
  }

  u32 unit_;
  std::array<std::atomic<std::byte*>, blocks> blocks_{};
  std::atomic<u32> size_{0};
  std::mutex m_;
  u32 cursor_ = 0;  // under m_
};

// A typed array over `Chunked`, of trivially copyable values, read while others append.
template <class T>
  requires std::is_trivially_copyable_v<T>
class Column {
public:
  u32 size() const noexcept { return c_.size(); }
  T get(u32 i) const noexcept {
    T v;
    std::memcpy(&v, c_.at(i), sizeof(T));
    return v;
  }
  void put(u32 i, const T& v) {
    c_.ensure(i);
    std::memcpy(c_.at(i), &v, sizeof(T));
  }
  u32 push(const T& v) {
    const u32 i = c_.alloc(1);
    std::memcpy(c_.at(i), &v, sizeof(T));
    return i;
  }
  // `xs` in one contiguous range; its offset.
  u32 append(std::span<const T> xs) {
    const u32 at = c_.alloc(static_cast<u32>(xs.size()));
    if (!xs.empty()) std::memcpy(c_.at(at), xs.data(), xs.size_bytes());
    return at;
  }
  std::span<const T> span(u32 offset, u32 n) const noexcept {
    if (n == 0) return {};
    // The block's bytes hold implicitly created `T`s (operator new, then memcpy).
    return {std::launder(reinterpret_cast<const T*>(c_.at(offset))), n};
  }

private:
  Chunked c_{sizeof(T)};
};

// Records by handle, each published once and complete (#156): a writer builds its record
// privately, then `publish` makes it visible with release; the first publication wins and later
// ones are dropped. A reader sees a record whole or not at all, without a lock, and a record never
// moves. Slots are in blocks that never move, as `Chunked`'s.
template <class T>
class Published {
public:
  Published() = default;
  Published(const Published&) = delete;
  Published& operator=(const Published&) = delete;
  ~Published() {
    for (std::size_t b = 0; b < blocks; ++b)
      if (Slot* p = blocks_[b].load(std::memory_order_relaxed)) {
        for (std::size_t i = 0; i < block_slots(b); ++i) delete p[i].load(std::memory_order_relaxed);
        delete[] p;
      }
  }

  // `v` at `i` unless a record is there already; the record at `i`.
  const T& publish(u32 i, T v) {
    Slot& s = slot(i);
    const T* cur = s.load(std::memory_order_acquire);
    if (cur) return *cur;
    auto fresh = std::make_unique<const T>(std::move(v));
    if (s.compare_exchange_strong(cur, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) return *fresh.release();
    return *cur;
  }
  const T* find(u32 i) const noexcept {
    const auto [b, off] = locate(i);
    if (b >= blocks) return nullptr;  // past every handle, as `none` is
    const Slot* p = blocks_[b].load(std::memory_order_acquire);
    return p ? p[off].load(std::memory_order_acquire) : nullptr;
  }

private:
  using Slot = std::atomic<const T*>;
  static constexpr unsigned first_bits = 10;
  static constexpr std::size_t blocks = 22;
  static constexpr std::size_t block_slots(std::size_t b) noexcept { return std::size_t{1} << (first_bits + b); }
  static constexpr std::pair<std::size_t, std::size_t> locate(u32 i) noexcept {
    const std::uint64_t biased = std::uint64_t{i} + block_slots(0);
    const std::size_t b = static_cast<std::size_t>(std::bit_width(biased)) - 1 - first_bits;
    return {b, static_cast<std::size_t>(biased - block_slots(b))};
  }
  Slot& slot(u32 i) {
    const auto [b, off] = locate(i);
    contract_assert(b < blocks);
    Slot* p = blocks_[b].load(std::memory_order_acquire);
    if (!p) {
      auto* fresh = new Slot[block_slots(b)]{};
      if (blocks_[b].compare_exchange_strong(p, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) p = fresh;
      else delete[] fresh;
    }
    return p[off];
  }

  std::array<std::atomic<Slot*>, blocks> blocks_{};
};

// A fingerprint table in shards, each under its own lock (§9.2: identity is the fingerprint).
template <class V>
class FpIndex {
public:
  std::optional<V> find(Fp128 f) const {
    Shard& s = shard(f);
    std::lock_guard lock(s.m);
    if (auto it = s.map.find(f); it != s.map.end()) return it->second;
    return std::nullopt;
  }
  // The value for `f`, or `make()`'s, inserted, under the shard's lock: of two workers interning
  // equal content, exactly one makes it. `make` takes no index lock (no interning, into any
  // table): two workers each holding a shard and waiting for the other's would deadlock. It may
  // append to `Chunked` stores, whose lock is a leaf.
  template <class F>
  std::pair<V, bool> find_or_insert(Fp128 f, F&& make) {
    Shard& s = shard(f);
    std::lock_guard lock(s.m);
    if (auto it = s.map.find(f); it != s.map.end()) return {it->second, false};
    V v = std::forward<F>(make)();
    s.map.emplace(f, v);
    return {v, true};
  }

private:
  struct alignas(64) Shard {
    // BOOTSTRAP: constexpr: a user-provided constructor, so that `Shard` is not constexpr-
    // constructible. At -O1 and above GCC 16.2 segfaults in an importer of a module whose class
    // has an inline user-provided constructor and two `FpIndex` specializations (`Interners`),
    // while constant-evaluating `shards_`'s initializer (#283, reduced to two files there;
    // related to GCC PR 124973). Without the bug this is an aggregate with no constructor:
    //   struct alignas(64) Shard { std::mutex m; std::unordered_map<Fp128, V, Fp128Hash> map; };
    Shard() {}
    std::mutex m;
    std::unordered_map<Fp128, V, Fp128Hash> map;
  };
  static constexpr std::size_t shard_count = 64;
  Shard& shard(Fp128 f) const noexcept { return (*shards_)[f.hi % shard_count]; }
  std::unique_ptr<std::array<Shard, shard_count>> shards_ = std::make_unique<std::array<Shard, shard_count>>();
};

}  // namespace ycxx::ast
