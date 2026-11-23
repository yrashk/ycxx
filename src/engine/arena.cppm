// An append-only array whose elements never move, readable while another thread appends
// (§A3.6): the memo table's cells, each kind's keys and values, the frame registry. An index is
// handed out once and its element constructed by the thread that took it; any other thread may
// read the element once it has learnt the index through something that synchronizes (the
// table's shard lock, or a state word published with release).
export module ycxx.engine:arena;

import std;
import :storage;

export namespace ycxx::engine {

template <class T>
class Arena {
public:
  explicit Arena(const ArenaBacking* backing = nullptr) : backing_(backing) {}
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() {
    const std::uint32_t n = size_.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < n; ++i) std::destroy_at(&(*this)[i]);
    for (std::size_t b = 0; b < blocks_.size(); ++b) {
      if (T* p = blocks_[b].load(std::memory_order_relaxed)) {
        release(p, block_size(b) * sizeof(T), mapped_[b]);
      }
    }
  }

  // Takes the next index and constructs its element. The constructor must not throw: an index
  // without an element would be destroyed.
  template <class... A>
    requires std::is_nothrow_constructible_v<T, A...>
  std::uint32_t emplace(A&&... args) {
    const std::uint32_t i = size_.fetch_add(1, std::memory_order_relaxed);
    contract_assert(i < max_size);
    const auto [b, at] = locate(i);
    std::construct_at(block(b) + at, std::forward<A>(args)...);
    return i;
  }

  T& operator[](std::uint32_t i) noexcept {
    const auto [b, at] = locate(i);
    return blocks_[b].load(std::memory_order_acquire)[at];
  }
  const T& operator[](std::uint32_t i) const noexcept {
    const auto [b, at] = locate(i);
    return blocks_[b].load(std::memory_order_acquire)[at];
  }
  // Indices handed out so far; at quiescence, the number of elements.
  std::uint32_t size() const noexcept { return size_.load(std::memory_order_acquire); }

  static constexpr std::uint32_t max_size = 0x80000000u;

private:
  // Block b holds `first << b` elements, so 22 blocks cover 2^31 indices.
  static constexpr unsigned first_bits = 10;
  static constexpr std::size_t block_size(std::size_t b) noexcept { return std::size_t{1} << (first_bits + b); }
  static constexpr std::pair<std::size_t, std::size_t> locate(std::uint32_t i) noexcept {
    const std::uint64_t biased = std::uint64_t{i} + (std::uint64_t{1} << first_bits);
    const std::size_t b = static_cast<std::size_t>(std::bit_width(biased)) - 1 - first_bits;
    return {b, static_cast<std::size_t>(biased - (std::uint64_t{1} << (first_bits + b)))};
  }
  static void release(T* p, std::size_t bytes, bool mapped) noexcept {
    if (mapped) ArenaBacking::release(p, bytes);
    else ::operator delete(static_cast<void*>(p), bytes, std::align_val_t{alignof(T)});
  }

  // The block, allocated by whichever thread needs it first.
  T* block(std::size_t b) {
    T* p = blocks_[b].load(std::memory_order_acquire);
    if (p) return p;
    const auto bytes = block_size(b) * sizeof(T);
    void* mapped = backing_ ? backing_->allocate(bytes, alignof(T)) : nullptr;
    auto* fresh = static_cast<T*>(mapped ? mapped : ::operator new(bytes, std::align_val_t{alignof(T)}));
    if (blocks_[b].compare_exchange_strong(p, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) {
      // Only the winner writes this flag. It is read solely when the arena is destroyed at
      // quiescence, so the array indexing path does not load it.
      mapped_[b] = mapped != nullptr;
      return fresh;
    }
    release(fresh, bytes, mapped != nullptr);
    return p;
  }

  const ArenaBacking* backing_;
  std::array<std::atomic<T*>, 22> blocks_{};
  std::array<bool, 22> mapped_{};
  std::atomic<std::uint32_t> size_{0};
};

}  // namespace ycxx::engine
