// The 128-bit fingerprint the AST's interners use as identity (docs/design/ast.md §9.2): a thin
// adapter over the engine's `ycxx.hash` (#14), so the AST and the engine fingerprint alike.
// Everything in src/ast goes through `Fp128`, `mix`, `mix_bytes` and `seed`.
export module ycxx.ast.fingerprint;

import std;
export import ycxx.hash;

export namespace ycxx::ast {

using ycxx::Fp128;

// Folds one word into a fingerprint. Order matters: mix(mix(h, a), b) != mix(mix(h, b), a).
constexpr Fp128 mix(Fp128 h, std::uint64_t v) { return Hasher().fp(h).uint(v).finish(); }

constexpr Fp128 mix(Fp128 h, Fp128 v) { return Hasher().fp(h).fp(v).finish(); }

constexpr Fp128 mix_bytes(Fp128 h, std::string_view bytes) {
  return Hasher().fp(h).uint(std::uint64_t{bytes.size()}).bytes(bytes).finish();
}

// A fingerprint seeded by a name, for schema seeds computed at compile time.
constexpr Fp128 seed(std::string_view name) { return fingerprint128(name); }

struct Fp128Hash {
  std::size_t operator()(Fp128 f) const noexcept { return static_cast<std::size_t>(f.lo); }
};

}  // namespace ycxx::ast
