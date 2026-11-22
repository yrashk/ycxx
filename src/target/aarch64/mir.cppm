// L3, the machine level (optimization design §2.1): AArch64 instructions by form, over virtual
// registers before allocation and physical ones after, with frame objects, symbols, labels and
// conditions as operands. Each instruction records the L2 node it came from, so the L3 origin map
// composes with L2's (§2.4).
export module ycxx.target.aarch64:mir;

import std;
import ycxx.ir;
import ycxx.ir.l2;
import :isa;

namespace ycxx::aarch64 {

export enum class MK : std::uint8_t {
  none,
  vreg,   // a virtual register
  preg,   // a physical register, an `R`
  imm,    // an immediate
  sym,    // an entity's symbol
  label,  // a block or edge label
  cond,   // a `Cond`
  frame,  // a frame object (an `alloca`)
  slot,   // a virtual register's stack slot (after allocation)
};

export struct MOp {
  MK kind = MK::none;
  std::int64_t v = 0;
  constexpr bool operator==(const MOp&) const = default;
  static constexpr MOp vreg(std::uint32_t v) { return {MK::vreg, v}; }
  static constexpr MOp preg(R r) { return {MK::preg, std::to_underlying(r)}; }
  static constexpr MOp imm(std::int64_t v) { return {MK::imm, v}; }
};

export struct MInst {
  Form form{};
  bool wide = true;        // 64-bit operation
  std::uint8_t access = 3; // memory forms: log2 of the access size
  std::uint8_t nops = 0;
  std::array<MOp, 4> ops{};
  std::uint32_t origin = ir::none;  // the L2 node
  constexpr bool operator==(const MInst&) const = default;
};

export struct MBlock {
  std::uint32_t label = 0;
  std::vector<MInst> insts;
};

export struct FrameObject {
  std::uint64_t size = 0;
  std::uint32_t align = 1;
};

export struct CleanupInvoke {
  ir::NodeId call;
  ir::BlockId landing;
};

export struct MFunction {
  std::vector<MBlock> blocks;      // in layout order; L2 blocks first, then edge blocks
  std::vector<l2::Type> vregs;     // each virtual register's type
  std::vector<FrameObject> objects;
  std::uint32_t labels = 0;
  std::uint32_t outgoing = 0;      // bytes of stack arguments the calls need, at sp
  bool allocated = false;          // no virtual registers remain
  std::optional<CleanupInvoke> cleanup;
};

export std::string print(const MFunction& f) {
  std::string out;
  auto o = std::back_inserter(out);
  for (const MBlock& b : f.blocks) {
    std::format_to(o, "L{}:\n", b.label);
    for (const MInst& in : b.insts) {
      std::format_to(o, "  {}", info(in.form).name);
      if (spec_of(in.form).size == Size::mem || in.form == Form::LDRfi || in.form == Form::STRfi)
        std::format_to(o, ".{}", 1 << in.access);
      else if (spec_of(in.form).size != Size::fixed) out += in.wide ? ".x" : ".w";
      for (std::uint8_t i = 0; i < in.nops; ++i) {
        const MOp& m = in.ops[i];
        out += i ? ", " : " ";
        switch (m.kind) {
          case MK::none: out += "_"; break;
          case MK::vreg: std::format_to(o, "v{}", m.v); break;
          case MK::preg: out += reg_name(R(m.v), true); break;
          case MK::imm: std::format_to(o, "#{}", m.v); break;
          case MK::sym: std::format_to(o, "@{}", m.v); break;
          case MK::label: std::format_to(o, "L{}", m.v); break;
          case MK::cond: out += cond_names[m.v]; break;
          case MK::frame: std::format_to(o, "fi{}", m.v); break;
          case MK::slot: std::format_to(o, "slot{}", m.v); break;
        }
      }
      out += '\n';
    }
  }
  return out;
}

}  // namespace ycxx::aarch64
