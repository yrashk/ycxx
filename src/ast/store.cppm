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
// `alloc(n)` hands out n contiguous units that never cross a block (the rest of a block is
// skipped instead), so a range is one span; with `alloc(1)` only, indices are dense. Moves only
// at quiescence (compaction).
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
  // Units handed out so far, skipped ones included.
  u32 size() const noexcept { return size_.load(std::memory_order_acquire); }

  u32 alloc(u32 n) {
    std::lock_guard lock(m_);
    u32 at = cursor_;
    if (n > 0) {
      const auto [b, off] = locate(at);
      if (off + n > block_units(b)) at = static_cast<u32>(block_base(b + 1));  // skip the block's tail
      contract_assert(locate(at).second + n <= block_units(locate(at).first));  // a range fits one block
      block(locate(at).first);
    }
    cursor_ = at + n;
    size_.store(cursor_, std::memory_order_release);
    return at;
  }

  // Makes unit `i` writable, for columns indexed by another pool's handle.
  void ensure(u32 i) { block(locate(i).first); }

  std::byte* at(u32 i) const noexcept {
    const auto [b, off] = locate(i);
    return blocks_[b].load(std::memory_order_acquire) + std::size_t(off) * unit_;
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
    return {reinterpret_cast<const T*>(c_.at(offset)), n};
  }

private:
  Chunked c_{sizeof(T)};
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
  // equal content, exactly one makes it. `make` must not intern into this table.
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
    std::mutex m;
    std::unordered_map<Fp128, V, Fp128Hash> map;
  };
  static constexpr std::size_t shard_count = 64;
  Shard& shard(Fp128 f) const noexcept { return (*shards_)[f.hi % shard_count]; }
  std::unique_ptr<std::array<Shard, shard_count>> shards_ = std::make_unique<std::array<Shard, shard_count>>();
};

}  // namespace ycxx::ast
