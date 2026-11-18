// The handles L1 uses to refer outside itself (AST design §2): entities, canonical types,
// constants and locations. They are opaque stand-ins until the AST core (#32, #33) provides the
// real ones; each is a strong 32-bit enum or, for `Loc`, an 8-byte structural pair, so wiring the
// real types in is a change of these declarations only.
//
// The body never hashes a handle's number: fingerprints go through a `handle_fingerprints`
// object (canon.cppm), because handles are local to the session (AST design §2, invariant 5).
export module ycxx.ir:handles;

import std;

namespace ycxx::ir {

export enum class EntityId : std::uint32_t {};
export enum class QualType : std::uint32_t {};
export enum class ConstId : std::uint32_t {};
// A block-scope variable, by its `LocalRec` index (AST design §7.1); used by function_debug only.
export enum class LocalId : std::uint32_t {};

// BOOTSTRAP: modules: comparisons in src/ir are defaulted members, not hidden friends. GCC 16.2
// crashes in an importer that uses a defaulted friend operator== of an exported class (STATUS).
export struct Loc {
  std::uint32_t item = 0;    // ItemId
  std::uint32_t offset = 0;  // token offset inside the item
  constexpr bool operator==(const Loc&) const = default;
};

export constexpr std::uint32_t raw(auto h)
  requires std::is_enum_v<decltype(h)>
{
  return std::to_underlying(h);
}

}  // namespace ycxx::ir
