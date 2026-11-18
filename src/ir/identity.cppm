// The fingerprint of an L1 body (optimization design §2.4): a Merkle hash of its canonical
// serialization. Ids inside the body are dense canonical indexes, so they are hashed as numbers;
// handles that leave the body (types, entities, constants) are hashed through their own
// fingerprints, supplied by a `handle_fingerprints` object, never by their session-local numbers
// (AST design §2, invariants 3 and 5). Nothing positional is part of a body, so neither is it of
// the fingerprint: locations live in `FunctionDebug`.
export module ycxx.ir:identity;

import std;
import :fingerprint;
import :handles;
import :ops;
import :function;

namespace ycxx::ir {

export template <class H>
concept handle_fingerprints = requires(const H& h, EntityId e, QualType t, ConstId c) {
  { h(e) } -> std::same_as<fingerprint128>;
  { h(t) } -> std::same_as<fingerprint128>;
  { h(c) } -> std::same_as<fingerprint128>;
};

// Until the AST core provides interners (#32, #33), a handle stands for itself.
export struct stand_in_handles {
  static fingerprint128 of(std::uint64_t tag, std::uint32_t v) { return hasher().add(tag).add(v).finish(); }
  fingerprint128 operator()(EntityId e) const { return of(1, raw(e)); }
  fingerprint128 operator()(QualType t) const { return of(2, raw(t)); }
  fingerprint128 operator()(ConstId c) const { return of(3, raw(c)); }
};

// Bumped whenever the serialization below or the op table's meaning changes.
export constexpr std::uint64_t l1_format_version = 1;

export template <handle_fingerprints H = stand_in_handles>
fingerprint128 fingerprint(const Function& f, const H& handles = {}) {
  hasher h;
  h.add(l1_format_version).add(f.blocks.size()).add(f.nodes.size());
  auto type = [&](Ty t) {
    h.add(std::to_underlying(t.kind));
    if (t.kind == TyKind::cxx) h.add(handles(t.type));
  };
  for (BlockId b : f.block_ids()) {
    h.add(f.block(b).nparams).add(f.block(b).nnodes);
    for (ValueId p : f.params_of(b)) type(f.value(p).type);
  }
  for (std::uint32_t i = 0; i < f.nodes.size(); ++i) {
    NodeId n{i};
    const Node& node = f.node(n);
    h.add(std::to_underlying(node.op)).add(node.flags).add(node.nres).add(node.nopnd);
    for (ValueId r : f.results_of(n)) type(f.value(r).type);
    for (ValueId v : f.operands_of(n)) h.add(raw(v));
    auto words = f.imms_of(n);
    std::size_t at = 0;
    for (Imm k : spec_of(node.op).imm.view()) {
      std::uint64_t w = words[at++];
      switch (k) {
        case Imm::type: h.add(handles(QualType(std::uint32_t(w)))); break;
        case Imm::entity: h.add(handles(EntityId(std::uint32_t(w)))); break;
        case Imm::constant: h.add(handles(ConstId(std::uint32_t(w)))); break;
        case Imm::consts:
          h.add(w);
          for (std::uint64_t j = 0; j < w; ++j) h.add(handles(ConstId(std::uint32_t(words[at++]))));
          break;
        case Imm::path:
          h.add(w);
          for (std::uint64_t j = 0; j < w; ++j) h.add(words[at++]);
          break;
        default: h.add(w); break;
      }
    }
    h.add(node.nsucc);
    for (const Succ& s : f.succs_of(n)) {
      h.add(raw(s.target)).add(s.nargs);
      for (ValueId v : f.args_of(s)) h.add(raw(v));
    }
  }
  return h.finish();
}

}  // namespace ycxx::ir
