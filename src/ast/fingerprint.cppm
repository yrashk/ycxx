// The 128-bit fingerprint the AST's interners use as identity (docs/design/ast.md §9.2).
//
// SEAM(#14): this is a stand-in. The query engine (#14) chooses the hash and exposes it as its
// own module; when it lands, this file becomes a re-export of that module and nothing else in
// src/ast changes. Everything here goes through `Fp128`, `mix`, `mix_bytes` and `seed`. Do not
// persist these fingerprints before then.
export module ycxx.ast.fingerprint;

import std;

export namespace ycxx::ast {

struct Fp128 {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
  constexpr bool operator==(const Fp128&) const = default;
};

namespace detail {
constexpr std::uint64_t fmix(std::uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}
}  // namespace detail

// Folds one word into a fingerprint. Order matters: mix(mix(h, a), b) != mix(mix(h, b), a).
constexpr Fp128 mix(Fp128 h, std::uint64_t v) {
  h.lo = detail::fmix(h.lo ^ v ^ 0x9e3779b97f4a7c15ULL);
  h.hi = detail::fmix(h.hi + v * 0xc2b2ae3d27d4eb4fULL + h.lo);
  return h;
}

constexpr Fp128 mix(Fp128 h, Fp128 v) { return mix(mix(h, v.lo), v.hi); }

constexpr Fp128 mix_bytes(Fp128 h, std::string_view bytes) {
  h = mix(h, bytes.size());
  for (char c : bytes) h = mix(h, static_cast<unsigned char>(c));
  return h;
}

// A fingerprint seeded by a name, for schema seeds computed at compile time.
constexpr Fp128 seed(std::string_view name) { return mix_bytes(Fp128{}, name); }

struct Fp128Hash {
  std::size_t operator()(Fp128 f) const noexcept { return static_cast<std::size_t>(f.lo); }
};

}  // namespace ycxx::ast
