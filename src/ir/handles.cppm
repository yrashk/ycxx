// The handles L1 uses to refer outside itself (AST design §2): entities, canonical types,
// constants and locations, from `ycxx.ast.handles`. `LocalId` is a stand-in until the AST has a
// `LocalRec` index type (§7.1).
//
// The body never hashes a handle's number: fingerprints go through a `handle_fingerprints`
// object (identity.cppm), because handles are local to the session (AST design §2, invariant 5).
export module ycxx.ir:handles;

import std;
import ycxx.ast.handles;

namespace ycxx::ir {

export using ast::ConstId;
export using ast::EntityId;
export using ast::ItemId;
export using ast::Loc;
export using ast::QualType;

// A block-scope variable, by its `LocalRec` index (AST design §7.1); used by function_debug only.
export enum class LocalId : std::uint32_t {};

export constexpr std::uint32_t raw(auto h)
  requires std::is_enum_v<decltype(h)>
{
  return std::to_underlying(h);
}

}  // namespace ycxx::ir
