// The AArch64 instruction forms code generation uses, as data (optimization design §5; #28): for
// each form, its fixed bits, how its width is chosen, and for each operand the role it plays
// (defined register, used register, immediate, symbol, label, condition, frame object) and the
// encoding field it fills. One generic encoder reads the table; nothing else builds instruction
// words. Fields and fixed bits are those of the Arm Architecture Reference Manual (DDI 0487),
// section C6 ("A64 Base Instruction Descriptions"), each form named after its instruction there.
//
// Pseudo forms (`MOVi`, `CSET`, `MOVrr`, the frame accesses, `RETp`) have no encoding: frame
// layout (emit.cppm) expands them into real forms once frame offsets are known.
export module ycxx.target.aarch64:isa;

import std;
import ycxx.ir;

namespace ycxx::aarch64 {

// Registers by their encoding number; `sp` and `zr` share 31 and are told apart by the operand.
// The SIMD and floating-point registers follow, `v0` to `v31`, encoded 0 to 31 by forms whose
// operands are of that class (C1.2.5); a value uses its low 32 (S) or 64 (D) bits.
export enum class R : std::uint8_t {
  x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17,
  x18, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, fp, lr, sp, zr,
  v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15,
  v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31,
};
export constexpr bool is_fp(R r) { return r >= R::v0; }
export constexpr R vreg_n(std::uint32_t n) { return R(std::uint32_t(R::v0) + n); }
export constexpr std::uint32_t enc(R r) {
  return r == R::zr ? 31u : is_fp(r) ? std::uint32_t(r) - std::uint32_t(R::v0) : std::uint32_t(r);
}
export std::string reg_name(R r, bool wide) {
  if (is_fp(r)) return std::format("{}{}", wide ? 'd' : 's', enc(r));
  if (r == R::sp) return wide ? "sp" : "wsp";
  if (r == R::zr) return wide ? "xzr" : "wzr";
  if (r == R::fp) return "x29";
  if (r == R::lr) return "x30";
  return std::format("{}{}", wide ? 'x' : 'w', std::to_underlying(r));
}

// Condition codes (C1.2.4).
export enum class Cond : std::uint8_t { eq, ne, hs, lo, mi, pl, vs, vc, hi, ls, ge, lt, gt, le, al };
export constexpr Cond invert(Cond c) { return Cond(std::to_underlying(c) ^ 1); }
export constexpr std::string_view cond_names[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                                  "hi", "ls", "ge", "lt", "gt", "le", "al"};

export enum class Role : std::uint8_t { def, use, imm, sym, label, cond, frame };

// Encoding fields: (least significant bit, width), and how the operand's value is placed.
export enum class Field : std::uint8_t {
  none,     // contributes no bits (a symbol whose bits come from a relocation; a frame object)
  rd, rn, rm, ra, rt2,
  imm12,    // [21:10], unsigned
  uoff12,   // [21:10], an offset in bytes divided by the access size
  sh12,     // [22], 1 to shift imm12 left by 12
  imm16,    // [20:5]
  hw,       // [22:21], the halfword index
  immr,     // [21:16]
  imms,     // [15:10]
  cc,       // [15:12]
  bcond,    // [3:0]
  rel19,    // [23:5], a byte offset from the instruction, divided by 4
  rel26,    // [25:0], likewise
  rel14,    // [18:5], likewise
  tbit,     // the bit number of TBZ/TBNZ: b5 at [31], b40 at [23:19]
  imm7x8,   // [21:15], a signed byte offset divided by 8 (64-bit pairs)
};

// How the width (`sf`) or access size bits are chosen.
export enum class Size : std::uint8_t {
  fixed,  // as in `bits`
  sf,     // bit 31 set for 64-bit operation
  sfn,    // bits 31 and 22 (N) set for 64-bit operation (bitfield moves)
  mem,    // bits [31:30] are log2 of the access size
  ftype,  // bit 22 (ftype 01) set for double precision, clear for single (C7.2)
  sfft,   // bits 31 and 22 set for X and D registers, clear for W and S (FMOV, general)
};

export enum class Reloc : std::uint8_t { none, page21, pageoff12, got_page21, got_pageoff12, branch26 };

export struct Opnd {
  Role role = Role::use;
  Field field = Field::none;
  bool fp = false;  // a SIMD&FP register
};

export struct FormSpec {
  std::uint32_t bits = 0;
  Size size = Size::fixed;
  ir::List<Opnd, 4> ops;
  Reloc reloc = Reloc::none;  // for the `sym` operand
  bool pseudo = false;
  bool branch = false;  // ends straight-line flow (an unconditional transfer)
};

namespace forms {
using enum Role;
using enum Field;
constexpr Opnd D(Field f = rd) { return {def, f}; }
constexpr Opnd U(Field f) { return {use, f}; }
constexpr Opnd DV(Field f = rd) { return {def, f, true}; }
constexpr Opnd UV(Field f) { return {use, f, true}; }
constexpr Opnd I(Field f) { return {imm, f}; }
constexpr Opnd S() { return {sym, Field::none}; }
constexpr Opnd L(Field f) { return {label, f}; }
constexpr Opnd C(Field f) { return {cond, f}; }
constexpr Opnd F() { return {frame, Field::none}; }
constexpr FormSpec f(std::uint32_t bits, Size s, ir::List<Opnd, 4> ops, Reloc r = Reloc::none) {
  return {bits, s, ops, r, false, false};
}
constexpr FormSpec pseudo(ir::List<Opnd, 4> ops) { return {0, Size::sf, ops, Reloc::none, true, false}; }
constexpr FormSpec branch(FormSpec s) { s.branch = true; return s; }
}  // namespace forms

using namespace forms;

export enum class Form : std::uint16_t {
  // Data processing, register (C6.2: ADD/SUB (shifted register), logical (shifted register)).
  ADDrr [[=f(0x0B000000, Size::sf, {D(), U(rn), U(rm)})]],
  SUBrr [[=f(0x4B000000, Size::sf, {D(), U(rn), U(rm)})]],
  SUBSrr [[=f(0x6B000000, Size::sf, {D(), U(rn), U(rm)})]],  // CMP when Rd is zr
  ANDrr [[=f(0x0A000000, Size::sf, {D(), U(rn), U(rm)})]],
  ORRrr [[=f(0x2A000000, Size::sf, {D(), U(rn), U(rm)})]],
  EORrr [[=f(0x4A000000, Size::sf, {D(), U(rn), U(rm)})]],
  ORNrr [[=f(0x2A200000, Size::sf, {D(), U(rn), U(rm)})]],  // MVN when Rn is zr
  // ADD/SUB (immediate); Rd and Rn may be sp.
  ADDri [[=f(0x11000000, Size::sf, {D(), U(rn), I(imm12)})]],
  ADDri12 [[=f(0x11400000, Size::sf, {D(), U(rn), I(imm12)})]],  // imm12, LSL #12
  SUBri [[=f(0x51000000, Size::sf, {D(), U(rn), I(imm12)})]],
  SUBri12 [[=f(0x51400000, Size::sf, {D(), U(rn), I(imm12)})]],
  SUBSri [[=f(0x71000000, Size::sf, {D(), U(rn), I(imm12)})]],
  ADDlo [[=f(0x91000000, Size::fixed, {D(), U(rn), S()}, Reloc::pageoff12)]],  // :lo12:sym
  TST1 [[=f(0x72000000 | 31, Size::fixed, {U(rn)})]],  // ANDS wzr, Wn, #1
  // Data processing, 3 source and 2 source.
  MADD [[=f(0x1B000000, Size::sf, {D(), U(rn), U(rm), U(ra)})]],
  MSUB [[=f(0x1B008000, Size::sf, {D(), U(rn), U(rm), U(ra)})]],
  SDIV [[=f(0x1AC00C00, Size::sf, {D(), U(rn), U(rm)})]],
  UDIV [[=f(0x1AC00800, Size::sf, {D(), U(rn), U(rm)})]],
  LSLV [[=f(0x1AC02000, Size::sf, {D(), U(rn), U(rm)})]],
  LSRV [[=f(0x1AC02400, Size::sf, {D(), U(rn), U(rm)})]],
  ASRV [[=f(0x1AC02800, Size::sf, {D(), U(rn), U(rm)})]],
  // Conditional select.
  CSEL [[=f(0x1A800000, Size::sf, {D(), U(rn), U(rm), C(cc)})]],
  CSINC [[=f(0x1A800400, Size::sf, {D(), U(rn), U(rm), C(cc)})]],
  // Bitfield moves (SXTB, UXTB, SXTW, ... are aliases).
  SBFM [[=f(0x13000000, Size::sfn, {D(), U(rn), I(immr), I(imms)})]],
  UBFM [[=f(0x53000000, Size::sfn, {D(), U(rn), I(immr), I(imms)})]],
  // Move wide (immediate).
  MOVZ [[=f(0x52800000, Size::sf, {D(), I(imm16), I(hw)})]],
  MOVN [[=f(0x12800000, Size::sf, {D(), I(imm16), I(hw)})]],
  MOVK [[=f(0x72800000, Size::sf, {{use, rd}, I(imm16), I(hw)})]],  // reads and writes Rd
  // PC-relative addressing.
  ADRP [[=f(0x90000000, Size::fixed, {D(), S()}, Reloc::page21)]],
  ADRPgot [[=f(0x90000000, Size::fixed, {D(), S()}, Reloc::got_page21)]],
  // Loads and stores, unsigned offset; Rn may be sp.
  LDRui [[=f(0x39400000, Size::mem, {D(), U(rn), I(uoff12)})]],
  STRui [[=f(0x39000000, Size::mem, {U(rd), U(rn), I(uoff12)})]],
  LDRlo [[=f(0x39400000, Size::mem, {D(), U(rn), S()}, Reloc::pageoff12)]],
  STRlo [[=f(0x39000000, Size::mem, {U(rd), U(rn), S()}, Reloc::pageoff12)]],
  LDRgot [[=f(0xF9400000, Size::fixed, {D(), U(rn), S()}, Reloc::got_pageoff12)]],
  // Loads and stores of SIMD and floating-point registers, unsigned offset (C7.2: LDR, STR
  // (immediate, SIMD&FP)); frame layout uses these for a form above whose register is one.
  LDRuiF [[=f(0x3D400000, Size::mem, {DV(), U(rn), I(uoff12)})]],
  STRuiF [[=f(0x3D000000, Size::mem, {UV(rd), U(rn), I(uoff12)})]],
  LDRloF [[=f(0x3D400000, Size::mem, {DV(), U(rn), S()}, Reloc::pageoff12)]],
  STRloF [[=f(0x3D000000, Size::mem, {UV(rd), U(rn), S()}, Reloc::pageoff12)]],
  STPpre [[=f(0xA9800000, Size::fixed, {U(rd), U(rt2), D(rn), I(imm7x8)})]],   // 64-bit, pre-index
  LDPpost [[=f(0xA8C00000, Size::fixed, {D(rd), D(rt2), D(rn), I(imm7x8)})]],  // 64-bit, post-index
  // Floating point, scalar (C7.2), single or double precision by `wide`.
  FADD [[=f(0x1E202800, Size::ftype, {DV(), UV(rn), UV(rm)})]],
  FSUB [[=f(0x1E203800, Size::ftype, {DV(), UV(rn), UV(rm)})]],
  FMUL [[=f(0x1E200800, Size::ftype, {DV(), UV(rn), UV(rm)})]],
  FDIV [[=f(0x1E201800, Size::ftype, {DV(), UV(rn), UV(rm)})]],
  FNEG [[=f(0x1E214000, Size::ftype, {DV(), UV(rn)})]],
  FABS [[=f(0x1E20C000, Size::ftype, {DV(), UV(rn)})]],
  FMOVrr [[=f(0x1E204000, Size::ftype, {DV(), UV(rn)})]],     // FMOV (register)
  FCMP [[=f(0x1E202000, Size::ftype, {UV(rn), UV(rm)})]],
  FCSEL [[=f(0x1E200C00, Size::ftype, {DV(), UV(rn), UV(rm), C(cc)})]],
  FCVTsd [[=f(0x1E22C000, Size::fixed, {DV(), UV(rn)})]],     // FCVT Dd, Sn
  FCVTds [[=f(0x1E624000, Size::fixed, {DV(), UV(rn)})]],     // FCVT Sd, Dn
  // Conversions between floating point and integers: the letter is the floating operand's
  // precision, `wide` the integer's width.
  FCVTZSs [[=f(0x1E380000, Size::sf, {D(), UV(rn)})]],
  FCVTZSd [[=f(0x1E780000, Size::sf, {D(), UV(rn)})]],
  FCVTZUs [[=f(0x1E390000, Size::sf, {D(), UV(rn)})]],
  FCVTZUd [[=f(0x1E790000, Size::sf, {D(), UV(rn)})]],
  SCVTFs [[=f(0x1E220000, Size::sf, {DV(), U(rn)})]],
  SCVTFd [[=f(0x1E620000, Size::sf, {DV(), U(rn)})]],
  UCVTFs [[=f(0x1E230000, Size::sf, {DV(), U(rn)})]],
  UCVTFd [[=f(0x1E630000, Size::sf, {DV(), U(rn)})]],
  FMOVtoF [[=f(0x1E270000, Size::sfft, {DV(), U(rn)})]],     // FMOV Sd, Wn / Dd, Xn: the bits
  FMOVtoI [[=f(0x1E260000, Size::sfft, {D(), UV(rn)})]],     // FMOV Wd, Sn / Xd, Dn
  // Branches.
  B [[=branch(f(0x14000000, Size::fixed, {L(rel26)}))]],
  Bcc [[=f(0x54000000, Size::fixed, {C(bcond), L(rel19)})]],
  TBNZ [[=f(0x37000000, Size::fixed, {U(rd), I(tbit), L(rel14)})]],
  BL [[=f(0x94000000, Size::fixed, {S()}, Reloc::branch26)]],
  BLR [[=f(0xD63F0000, Size::fixed, {U(rn)})]],
  RET [[=branch(f(0xD65F03C0, Size::fixed, {}))]],
  BRK [[=branch(f(0xD4200000, Size::fixed, {I(imm16)}))]],

  // Pseudo forms, expanded by frame layout.
  MOVi [[=pseudo({D(), I(none)})]],                  // any 64-bit constant
  CSET [[=pseudo({D(), C(none)})]],                  // CSINC Rd, zr, zr, invert(cond)
  MOVrr [[=pseudo({D(), U(none)})]],                 // ORR Rd, zr, Rm, or ADD #0 with sp
  ADDfi [[=pseudo({D(), F(), I(none)})]],            // the address of a frame object plus an offset
  LDRfi [[=pseudo({D(), F(), I(none)})]],            // a load from a frame object plus an offset
  STRfi [[=pseudo({U(none), F(), I(none)})]],        // a store to it
  RETp [[=branch(pseudo({}))]],                      // the epilogue and RET
};

export constexpr std::size_t form_count = std::meta::enumerators_of(^^Form).size();

export struct FormInfo {
  std::string_view name;
  FormSpec spec;
};

consteval std::array<FormInfo, form_count> build_forms() {
  std::array<FormInfo, form_count> t{};
  std::size_t i = 0;
  for (auto e : std::meta::enumerators_of(^^Form)) {
    auto a = std::meta::annotations_of_with_type(e, ^^FormSpec);
    if (a.size() != 1) throw "every Form needs exactly one FormSpec annotation";
    t[i].name = std::meta::identifier_of(e);
    t[i].spec = std::meta::extract<FormSpec>(a[0]);
    ++i;
  }
  return t;
}

export constexpr std::array<FormInfo, form_count> form_table = build_forms();
export constexpr const FormInfo& info(Form f) { return form_table[std::to_underlying(f)]; }
export constexpr const FormSpec& spec_of(Form f) { return info(f).spec; }

export constexpr std::optional<Form> form_named(std::string_view name) {
  for (std::size_t i = 0; i < form_count; ++i)
    if (form_table[i].name == name) return Form(i);
  return std::nullopt;
}

// One encodable instruction: a real form with every operand resolved. Registers are `R`
// numbers, labels are byte offsets from the instruction, and `access` is log2 of the access size
// of a memory form.
export struct Encodable {
  Form form{};
  bool wide = true;
  std::uint8_t access = 3;
  std::array<std::int64_t, 4> ops{};
};

constexpr std::pair<std::uint32_t, std::uint32_t> place(Field f) {  // (lsb, width)
  switch (f) {
    case Field::rd: return {0, 5};
    case Field::rn: return {5, 5};
    case Field::rm: return {16, 5};
    case Field::ra:
    case Field::rt2: return {10, 5};
    case Field::imm12:
    case Field::uoff12: return {10, 12};
    case Field::sh12: return {22, 1};
    case Field::imm16: return {5, 16};
    case Field::hw: return {21, 2};
    case Field::immr: return {16, 6};
    case Field::imms: return {10, 6};
    case Field::cc: return {12, 4};
    case Field::bcond: return {0, 4};
    case Field::rel19: return {5, 19};
    case Field::rel26: return {0, 26};
    case Field::rel14: return {5, 14};
    case Field::imm7x8: return {15, 7};
    case Field::tbit:
    case Field::none: return {0, 0};
  }
  return {0, 0};
}

// The instruction word, or why the operands do not fit their fields.
export std::expected<std::uint32_t, std::string> encode(const Encodable& in) {
  const FormSpec& s = spec_of(in.form);
  if (s.pseudo) return std::unexpected(std::format("{} is a pseudo form", info(in.form).name));
  std::uint32_t w = s.bits;
  switch (s.size) {
    case Size::fixed: break;
    case Size::sf: if (in.wide) w |= 1u << 31; break;
    case Size::sfn: if (in.wide) w |= 1u << 31 | 1u << 22; break;
    case Size::mem: w |= std::uint32_t(in.access) << 30; break;
    case Size::ftype: if (in.wide) w |= 1u << 22; break;
    case Size::sfft: if (in.wide) w |= 1u << 31 | 1u << 22; break;
  }
  auto ops = s.ops.view();
  for (std::size_t i = 0; i < ops.size(); ++i) {
    std::int64_t v = in.ops[i];
    Field f = ops[i].field;
    auto fail = [&](std::string_view why) {
      return std::unexpected(std::format("{} operand {}: {} ({})", info(in.form).name, i, why, v));
    };
    switch (f) {
      case Field::none: continue;
      case Field::uoff12: {
        std::int64_t scale = std::int64_t(1) << in.access;
        if (v < 0 || v % scale) return fail("the offset is not a non-negative multiple of the access size");
        v /= scale;
        break;
      }
      case Field::rel19:
      case Field::rel26:
      case Field::rel14:
      case Field::imm7x8: {
        std::int64_t scale = f == Field::imm7x8 ? 8 : 4;
        if (v % scale) return fail("misaligned offset");
        v /= scale;
        auto [lsb, width] = place(f);
        std::int64_t lim = std::int64_t(1) << (width - 1);
        if (v < -lim || v >= lim) return fail("offset out of range");
        v &= (std::int64_t(1) << width) - 1;
        w |= std::uint32_t(v) << lsb;
        continue;
      }
      case Field::tbit:
        if (v < 0 || v > 63) return fail("bit number out of range");
        w |= std::uint32_t(v >> 5) << 31 | std::uint32_t(v & 31) << 19;
        continue;
      default: break;
    }
    auto [lsb, width] = place(f);
    if (v < 0 || v >= (std::int64_t(1) << width)) return fail("value out of range");
    w |= std::uint32_t(v) << lsb;
  }
  return w;
}

}  // namespace ycxx::aarch64
