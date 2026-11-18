// A stand-in for the engine's 128-bit fingerprint (#14 chooses the hash, query-engine §A1.3). The
// IR only needs a value type and a streaming hasher; when the engine's lands, this partition
// re-exports it and nothing else in src/ir changes.
//
// The mixer is splitmix64's finalizer over two lanes with different absorption, which is enough
// for tests to tell bodies apart. It is not the published hash §A1.3 asks for.
export module ycxx.ir:fingerprint;

import std;

namespace ycxx::ir {

export struct fingerprint128 {
  std::uint64_t lo = 0, hi = 0;
  constexpr bool operator==(const fingerprint128&) const = default;
  constexpr auto operator<=>(const fingerprint128&) const = default;
};

constexpr std::uint64_t mix(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
  return z ^ (z >> 31);
}

export class hasher {
 public:
  constexpr hasher& add(std::uint64_t x) {
    a_ = mix(a_ ^ x) + 0x9e3779b97f4a7c15u;
    b_ = mix(b_ + x * 0xd6e8feb86659fd93u) ^ a_;
    ++n_;
    return *this;
  }
  constexpr hasher& add(fingerprint128 f) { return add(f.lo).add(f.hi); }
  constexpr fingerprint128 finish() const { return {mix(a_ ^ n_), mix(b_ + n_)}; }

 private:
  std::uint64_t a_ = 0x243f6a8885a308d3u, b_ = 0x13198a2e03707344u, n_ = 0;
};

}  // namespace ycxx::ir
