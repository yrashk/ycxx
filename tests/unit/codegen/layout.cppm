// A stand-in for the `layout` query in the back-end tests, until types and constants (#33) exist.
// Types are numbered as the tests' L1 text spells them (`T<n>`), and a constant's id is its value
// read as a 32-bit signed integer (`#4294967295` is -1), except for the 64-bit constants
// numbered from `#2130706432` (0x7f000000), and the address constants from `#2130706688`
// (0x7f000100), which is the address of the object @100, then @101, and so on.
export module ycxx.unit.codegen;

import std;
import ycxx.ir;
import ycxx.ir.lower;

export namespace ycxx::unit {

// T1 int, T2 bool, T3 long, T4 unsigned, T5 char (signed on arm64 macOS), T6 const char*,
// T7 int*, T8 char[16], T9 char(*)[16], T10 unsigned char, T11 struct { int a; long b; },
// T12 T11*, T13 short, T14 unsigned long, T15 const char**, T16 bool*, T17 long*, T18 int(*)(int),
// T20 char[40000], T21 char(*)[40000].
struct TestLayout {
  ir::TypeLayout type(ir::QualType t) const {
    using enum ir::TypeKind;
    switch (ir::raw(t)) {
      case 1: return {integer, 4, 4, true, {}};
      case 2: return {boolean, 1, 1, false, {}};
      case 3: return {integer, 8, 8, true, {}};
      case 4: return {integer, 4, 4, false, {}};
      case 5: return {integer, 1, 1, true, {}};
      case 6: return {pointer, 8, 8, false, ir::QualType(5)};
      case 7: return {pointer, 8, 8, false, ir::QualType(1)};
      case 8: return {array, 16, 1, false, {}};
      case 9: return {pointer, 8, 8, false, ir::QualType(8)};
      case 10: return {integer, 1, 1, false, {}};
      case 11: return {record, 16, 8, false, {}};
      case 12: return {pointer, 8, 8, false, ir::QualType(11)};
      case 13: return {integer, 2, 2, true, {}};
      case 14: return {integer, 8, 8, false, {}};
      case 15: return {pointer, 8, 8, false, ir::QualType(6)};
      case 16: return {pointer, 8, 8, false, ir::QualType(2)};
      case 17: return {pointer, 8, 8, false, ir::QualType(3)};
      case 18: return {pointer, 8, 8, false, ir::QualType(19)};
      case 19: return {function, 0, 1, false, {}};
      case 20: return {array, 40000, 1, false, {}};
      case 21: return {pointer, 8, 8, false, ir::QualType(20)};
      default: return {};
    }
  }
  ir::SubObject step(ir::QualType t, ir::PathStep s) const {
    if (ir::raw(t) == 8 || ir::raw(t) == 20) return {s.index, ir::QualType(5)};
    if (ir::raw(t) == 11) return s.index == 0 ? ir::SubObject{0, ir::QualType(1)} : ir::SubObject{8, ir::QualType(3)};
    return {};
  }
  ir::ConstValue constant(ir::ConstId c) const {
    static constexpr std::int64_t wide[] = {0x123456789abcdef0, -4294967297};
    std::uint32_t i = ir::raw(c) - 0x7f000000u;
    if (i < std::size(wide)) return wide[i];
    if (i >= 0x100 && i < 0x200) return {ir::EntityId(i - 0x100 + 100), 0};  // the address of @100 + n
    return std::int32_t(ir::raw(c));
  }
};

static_assert(ir::layout_source<TestLayout>);

}  // namespace ycxx::unit
