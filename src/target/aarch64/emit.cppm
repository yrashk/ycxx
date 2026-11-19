// Register allocation by stack slots, frame layout, and encoding (optimization design §5.2; #28).
//
// The first allocator gives every virtual register an 8-byte stack slot (`-O0`-like and
// debuggable): each instruction loads its operands from their slots into the convention's
// scratch registers, floating ones into its floating scratch registers, and stores its result
// back. SSA linear scan replaces it at level 1, and the allocation validator (#29) checks both.
//
// A form's register operands are general-purpose registers. Where a floating value takes their
// place, expansion picks the form's floating counterpart: a load or store of a SIMD&FP register,
// FCSEL for CSEL, FMOV for MOVrr and, after a MOVi, FMOV from the integer register built.
//
// The frame, from sp upwards: the outgoing stack arguments, the slots, the frame objects
// (`alloca`), then the frame record (x29, x30), which x29 points at:
//
//   [x29 + 16 ...]  incoming stack arguments
//   [x29]           saved x29, x30
//   ...             frame objects, slots
//   [sp]            outgoing stack arguments
//
// Frame layout then expands the pseudo forms, places the labels and encodes each instruction into
// the object's text section, with a relocation for each symbol operand.
export module ycxx.target.aarch64:emit;

import std;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.obj;
import :isa;
import :abi;
import :mir;

namespace ycxx::aarch64 {

// Rewrites `f` to physical registers and slots.
export void allocate_slots(MFunction& f, const CallingConvention& cc = apple_arm64);

// Lays out the frame and appends the function's code to `text`. `symbol(e)` gives the object's
// symbol index for an entity. Fails on an operand that does not fit (a branch out of range).
export std::expected<void, std::string> emit(const MFunction& f, obj::section& text,
                                             std::function_ref<std::uint32_t(ir::EntityId)> symbol,
                                             const CallingConvention& cc = apple_arm64);

}  // namespace ycxx::aarch64

namespace ycxx::aarch64 {

void allocate_slots(MFunction& f, const CallingConvention& cc) {
  for (MBlock& b : f.blocks) {
    std::vector<MInst> out;
    for (const MInst& in : b.insts) {
      MInst r = in;
      std::vector<MInst> after;
      std::size_t next = 0, next_fp = 0;
      auto ops = spec_of(in.form).ops.view();
      auto fp = [&](std::uint8_t i) { return l2::is_float(f.vregs[in.ops[i].v]); };
      for (std::uint8_t i = 0; i < in.nops; ++i) {
        if (in.ops[i].kind != MK::vreg || ops[i].role != Role::use) continue;
        R s = fp(i) ? cc.fp_scratch[next_fp++] : cc.scratch[next++];
        out.push_back({Form::LDRfi, true, 3, 3, {MOp::preg(s), {MK::slot, in.ops[i].v}, MOp::imm(0)}, in.origin});
        r.ops[i] = MOp::preg(s);
      }
      for (std::uint8_t i = 0; i < in.nops; ++i) {
        if (in.ops[i].kind != MK::vreg || ops[i].role != Role::def) continue;
        R s = fp(i) ? cc.fp_scratch[0] : cc.scratch[0];
        after.push_back({Form::STRfi, true, 3, 3, {MOp::preg(s), {MK::slot, in.ops[i].v}, MOp::imm(0)}, in.origin});
        r.ops[i] = MOp::preg(s);
      }
      out.push_back(r);
      out.insert(out.end(), after.begin(), after.end());
    }
    b.insts = std::move(out);
  }
  f.allocated = true;
}

namespace {

struct Frame {
  std::uint64_t size = 0;  // below the frame record, a multiple of 16
  std::uint64_t slot0 = 0;
  std::vector<std::uint64_t> object;
};

Frame layout(const MFunction& f, const CallingConvention& cc) {
  Frame fr;
  std::uint64_t at = f.outgoing;
  fr.slot0 = at;
  at += 8 * f.vregs.size();
  for (const FrameObject& o : f.objects) {
    std::uint64_t a = std::max<std::uint64_t>(o.align, 1);
    at = (at + a - 1) / a * a;
    fr.object.push_back(at);
    at += std::max<std::uint64_t>(o.size, 1);
  }
  fr.size = (at + cc.stack_align - 1) / cc.stack_align * cc.stack_align;
  return fr;
}

// Instructions with resolved registers and immediates; labels and symbols still symbolic.
struct Real {
  Encodable e;
  std::int64_t label = -1;   // operand index `label_op` is a label
  std::uint8_t label_op = 0;
  std::int64_t sym = -1;     // an entity
  Reloc reloc = Reloc::none;
};

class Expander {
 public:
  Expander(const MFunction& f, const Frame& fr, const CallingConvention& cc) : f_(f), fr_(fr), cc_(cc) {}
  std::vector<Real> code;
  std::vector<std::uint64_t> label_at;  // instruction index of each label
  std::string error;

  void run() {
    label_at.assign(f_.labels, 0);
    prologue();
    for (const MBlock& b : f_.blocks) {
      label_at[b.label] = code.size();
      for (const MInst& in : b.insts) inst(in);
    }
  }

 private:
  const MFunction& f_;
  const Frame& fr_;
  const CallingConvention& cc_;

  void put(Form form, bool wide, std::initializer_list<std::int64_t> ops, std::uint8_t access = 3) {
    Real r;
    r.e.form = form;
    r.e.wide = wide;
    r.e.access = access;
    std::size_t i = 0;
    for (auto v : ops) r.e.ops[i++] = v;
    code.push_back(r);
  }
  static std::int64_t reg(const MOp& m) { return enc(R(m.v)); }

  // d = base + off, for a non-negative offset below 2^24.
  void add_imm(R d, R base, std::uint64_t off) {
    if (off >> 24) {
      error = "a frame larger than 16 MiB is not supported";
      return;
    }
    std::uint64_t hi = off >> 12, lo = off & 0xfff;
    if (hi) put(Form::ADDri12, true, {enc(d), enc(base), std::int64_t(hi)});
    if (lo || !hi) put(Form::ADDri, true, {enc(d), enc(hi ? d : base), std::int64_t(lo)});
  }

  void prologue() {
    put(Form::STPpre, true, {enc(R::fp), enc(R::lr), enc(R::sp), -std::int64_t(cc_.frame_record)});
    put(Form::ADDri, true, {enc(R::fp), enc(R::sp), 0});
    std::uint64_t rest = fr_.size;
    if (rest >> 24) error = "a frame larger than 16 MiB is not supported";
    // A page at a time, touching each, so a large frame cannot step over the guard page.
    while (rest > cc_.page) {
      put(Form::SUBri12, true, {enc(R::sp), enc(R::sp), std::int64_t(cc_.page >> 12)});
      put(Form::STRui, true, {enc(R::zr), enc(R::sp), 0}, 3);
      rest -= cc_.page;
    }
    std::uint64_t hi = rest >> 12, lo = rest & 0xfff;
    if (hi) put(Form::SUBri12, true, {enc(R::sp), enc(R::sp), std::int64_t(hi)});
    if (lo) put(Form::SUBri, true, {enc(R::sp), enc(R::sp), std::int64_t(lo)});
  }

  std::uint64_t frame_address(const MOp& m) const {
    if (m.kind == MK::slot) return fr_.slot0 + 8 * std::uint64_t(m.v);
    return fr_.object[m.v];
  }

  // A load or store of `access` at sp + off.
  void frame_access(bool load, R r, std::uint64_t off, std::uint8_t access) {
    Form form = is_fp(r) ? (load ? Form::LDRuiF : Form::STRuiF) : (load ? Form::LDRui : Form::STRui);
    std::uint64_t s = std::uint64_t(1) << access;
    if (off % s == 0 && off / s < 4096) {
      put(form, true, {enc(r), enc(R::sp), std::int64_t(off)}, access);
      return;
    }
    add_imm(cc_.ip0, R::sp, off);
    put(form, true, {enc(r), enc(cc_.ip0), 0}, access);
  }

  // The floating counterpart of a form whose register operand is a SIMD&FP register.
  static Form fp_form(Form f) {
    switch (f) {
      case Form::LDRui: return Form::LDRuiF;
      case Form::STRui: return Form::STRuiF;
      case Form::LDRlo: return Form::LDRloF;
      case Form::STRlo: return Form::STRloF;
      case Form::CSEL: return Form::FCSEL;
      default: return f;
    }
  }

  // MOVZ or MOVN, then MOVK for each remaining halfword (C6.2, "MOV (wide immediate)").
  void mov_imm(std::int64_t d, std::int64_t value, bool wide) {
    std::uint64_t v = std::uint64_t(value);
    unsigned n = wide ? 4 : 2;
    if (!wide) v &= 0xffffffffu;
    auto half = [&](unsigned i) { return (v >> (16 * i)) & 0xffff; };
    unsigned zeros = 0, ones = 0;
    for (unsigned i = 0; i < n; ++i) {
      zeros += half(i) == 0;
      ones += half(i) == 0xffff;
    }
    bool inverted = ones > zeros;
    std::uint64_t fill = inverted ? 0xffff : 0;
    bool first = true;
    for (unsigned i = 0; i < n; ++i) {
      if (half(i) == fill) continue;
      if (first) {
        std::uint64_t h = inverted ? (~half(i) & 0xffff) : half(i);
        put(inverted ? Form::MOVN : Form::MOVZ, wide, {d, std::int64_t(h), i});
        first = false;
      } else {
        put(Form::MOVK, wide, {d, std::int64_t(half(i)), i});
      }
    }
    if (first) put(inverted ? Form::MOVN : Form::MOVZ, wide, {d, 0, 0});
  }

  void inst(const MInst& in) {
    auto op = [&](std::size_t i) { return in.ops[i]; };
    switch (in.form) {
      case Form::MOVi:
        // A floating constant's bits are built in ip0 and moved over.
        if (is_fp(R(op(0).v))) {
          mov_imm(enc(cc_.ip0), op(1).v, in.wide);
          put(Form::FMOVtoF, in.wide, {reg(op(0)), enc(cc_.ip0)});
          return;
        }
        mov_imm(reg(op(0)), op(1).v, in.wide);
        return;
      case Form::CSET:
        put(Form::CSINC, in.wide, {reg(op(0)), enc(R::zr), enc(R::zr), std::to_underlying(invert(Cond(op(1).v)))});
        return;
      case Form::MOVrr:
        if (is_fp(R(op(0).v)) && is_fp(R(op(1).v))) put(Form::FMOVrr, true, {reg(op(0)), reg(op(1))});
        else if (is_fp(R(op(0).v))) put(Form::FMOVtoF, true, {reg(op(0)), reg(op(1))});
        else if (is_fp(R(op(1).v))) put(Form::FMOVtoI, true, {reg(op(0)), reg(op(1))});
        else if (R(op(0).v) == R::sp || R(op(1).v) == R::sp) put(Form::ADDri, true, {reg(op(0)), reg(op(1)), 0});
        else put(Form::ORRrr, in.wide, {reg(op(0)), enc(R::zr), reg(op(1))});
        return;
      case Form::ADDfi: {
        if (op(2).v < 0) {
          error = "a negative frame offset";
          return;
        }
        add_imm(R(op(0).v), R::sp, frame_address(op(1)) + std::uint64_t(op(2).v));
        return;
      }
      case Form::LDRfi:
      case Form::STRfi:
        frame_access(in.form == Form::LDRfi, R(op(0).v), frame_address(op(1)) + std::uint64_t(op(2).v), in.access);
        return;
      case Form::RETp:
        put(Form::ADDri, true, {enc(R::sp), enc(R::fp), 0});
        put(Form::LDPpost, true, {enc(R::fp), enc(R::lr), enc(R::sp), std::int64_t(cc_.frame_record)});
        put(Form::RET, true, {});
        return;
      default: break;
    }
    Real r;
    r.e.form = in.form;
    for (std::uint8_t i = 0; i < in.nops; ++i)
      if (in.ops[i].kind == MK::preg && is_fp(R(in.ops[i].v))) r.e.form = fp_form(in.form);
    r.e.wide = in.wide;
    r.e.access = in.access;
    const FormSpec& s = spec_of(r.e.form);
    for (std::uint8_t i = 0; i < in.nops; ++i) {
      const MOp& m = in.ops[i];
      switch (m.kind) {
        case MK::preg: r.e.ops[i] = reg(m); break;
        case MK::imm:
        case MK::cond: r.e.ops[i] = m.v; break;
        case MK::label:
          r.label = m.v;
          r.label_op = i;
          break;
        case MK::sym:
          r.sym = m.v;
          r.reloc = s.reloc;
          break;
        default: error = std::format("{}: operand {} is not allocated", info(r.e.form).name, i); return;
      }
    }
    code.push_back(r);
  }
};

}  // namespace

std::expected<void, std::string> emit(const MFunction& f, obj::section& text,
                                      std::function_ref<std::uint32_t(ir::EntityId)> symbol,
                                      const CallingConvention& cc) {
  if (!f.allocated) return std::unexpected(std::string("emit needs an allocated function"));
  for (const FrameObject& o : f.objects)
    if (o.align > cc.stack_align)
      return std::unexpected(std::format("a frame object aligned to {} bytes is not supported (sp is aligned to {})",
                                         o.align, cc.stack_align));
  Frame fr = layout(f, cc);
  Expander x(f, fr, cc);
  x.run();
  if (!x.error.empty()) return std::unexpected(x.error);
  text.align(2);
  for (std::size_t i = 0; i < x.code.size(); ++i) {
    Real r = x.code[i];
    if (r.label >= 0) r.e.ops[r.label_op] = (std::int64_t(x.label_at[r.label]) - std::int64_t(i)) * 4;
    auto w = encode(r.e);
    if (!w) return std::unexpected(w.error());
    text.emit32(*w);
    if (r.sym >= 0) {
      obj::reloc_kind k{};
      switch (r.reloc) {
        case Reloc::page21: k = obj::reloc_kind::page21; break;
        case Reloc::pageoff12: k = obj::reloc_kind::pageoff12; break;
        case Reloc::got_page21: k = obj::reloc_kind::got_page21; break;
        case Reloc::got_pageoff12: k = obj::reloc_kind::got_pageoff12; break;
        case Reloc::branch26: k = obj::reloc_kind::branch26; break;
        case Reloc::none: return std::unexpected(std::string("a symbol operand without a relocation"));
      }
      text.relocate(k, symbol(ir::EntityId(std::uint32_t(r.sym))));
    }
  }
  return {};
}

}  // namespace ycxx::aarch64
