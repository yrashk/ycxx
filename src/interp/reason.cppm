// Why an evaluation stopped without a value ([expr.const]): every operation the draft makes
// non-constant, every undefined behaviour the interpreter detects, and the limits it keeps. Each
// reason carries its message as an annotation, so the table is derived by reflection, as sema's
// diagnostics are (ycxx.sema:diag). `{0}` in a message is the failure's argument.
export module ycxx.interp:reason;

import std;

export namespace ycxx::interp {

struct says {
  char text[112];
};

enum class Reason : std::uint8_t {
  // Limits ([implimits]).
  step_limit [[=says{"the evaluation exceeded its limit of {0} steps"}]],
  depth_limit [[=says{"the evaluation exceeded its limit of {0} nested calls"}]],
  // Arithmetic ([expr.pre], [expr.mul], [expr.shift]).
  signed_overflow [[=says{"signed integer overflow"}]],
  unsigned_overflow [[=says{"unsigned integer overflow in an operation that must not wrap"}]],
  division_by_zero [[=says{"division by zero"}]],
  quotient_overflow [[=says{"the quotient is not representable in the result type"}]],
  inexact [[=says{"an exact division or shift discarded nonzero bits"}]],
  shift_count [[=says{"shift count {0} is negative or not less than the width of the shifted type"}]],
  ctz_zero [[=says{"counting trailing zeros with a zero operand"}]],
  float_nan [[=says{"floating-point arithmetic produces a NaN"}]],
  float_to_integer [[=says{"a floating value converted to an integer type is out of its range"}]],
  enum_range [[=says{"the converted value is outside the range of the enumeration type"}]],
  // Objects ([basic.life], [basic.indet], [basic.lval], [dcl.type.cv]).
  read_indeterminate [[=says{"read of an object with an indeterminate value"}]],
  read_erroneous [[=says{"read of an object with an erroneous value"}]],
  outside_lifetime [[=says{"access to an object outside its lifetime"}]],
  storage_ended [[=says{"access to an object whose storage has ended"}]],
  null_access [[=says{"access through a null pointer"}]],
  past_end_access [[=says{"access through a pointer past the end of an object"}]],
  access_type [[=says{"access to an object through a glvalue of a different type"}]],
  modify_const [[=says{"modification of a const object"}]],
  volatile_object_access [[=says{"access to a volatile object through a non-volatile glvalue"}]],
  unsequenced [[=says{"unsequenced modification and access to the same object"}]],
  // Pointers ([expr.add], [expr.eq], [expr.rel], [expr.static.cast]).
  pointer_bounds [[=says{"pointer arithmetic leaves the bounds of the array"}]],
  pointer_type [[=says{"pointer arithmetic on a pointer to an object of a different type"}]],
  pointer_difference [[=says{"subtraction of pointers that do not point into the same array"}]],
  unspecified_comparison [[=says{"the result of comparing these pointers is unspecified"}]],
  void_cast_type [[=says{"conversion from 'void*' to a pointer to a type the object does not have"}]],
  // Builtins (docs/EXTENSIONS.md): their bytes are the values of objects of narrow character type.
  byte_type [[=says{"memchr or memcmp reads an object whose type is not a narrow character type"}]],
  // Operations [expr.const] makes non-constant.
  not_usable [[=says{"read of a variable that is not usable in constant expressions"}]],
  modify_outside [[=says{"modification of an object whose lifetime began outside the constant evaluation"}]],
  volatile_access [[=says{"access through a volatile glvalue"}]],
  not_constexpr [[=says{"call to a function that is not constexpr"}]],
  undefined_function [[=says{"call to a function that is not defined"}]],
  callee_unsupported [[=says{"the called function's body cannot be evaluated yet"}]],
  null_call [[=says{"call through a null function pointer"}]],
  reinterpret [[=says{"a reinterpret_cast or a conversion between a pointer and an integer"}]],
  asm_ [[=says{"an asm declaration"}]],
  goto_ [[=says{"a goto statement"}]],
  static_definition [[=says{"control passes through the definition of a static variable"}]],
  unreachable [[=says{"control reached the end of a function that must return a value"}]],
  explicit_trap [[=says{"execution reached __builtin_trap()"}]],
  // [utility.unreachable]: reaching it is undefined behavior.
  assumed_unreachable [[=says{"execution reached __builtin_unreachable()"}]],
  local_address [[=says{"the result points to an object with automatic storage duration"}]],
  // Not yet: an operation the interpreter does not evaluate, by its op number.
  unsupported [[=says{"the interpreter does not evaluate '{0}' yet"}]],
  // A constant the interpreter does not decode yet (an aggregate, an address with a path).
  unsupported_constant [[=says{"the interpreter does not evaluate this constant yet"}]],
  // The body broke an invariant `ir::verify` does not check (a value of the wrong kind).
  ill_formed [[=says{"internal error: the IR is ill-formed here"}]],
  unknown_dynamic_type [[=says{"a virtual call on an object whose dynamic type is constexpr-unknown"}]],
  immediate_dispatch_phase [[=says{"an immediate virtual call during construction or destruction is not supported yet"}]],
  virtual_dispatch_phase [[=says{"a virtual call during construction or destruction is not supported yet"}]],
};

consteval auto reason_enumerators() { return std::define_static_array(std::meta::enumerators_of(^^Reason)); }

constexpr std::string_view text_of(Reason r) {
  std::string_view s;
  template for (constexpr auto e : reason_enumerators()) {
    if (r == [:e:]) s = std::define_static_string(std::meta::extract<says>(std::meta::annotations_of_with_type(e, ^^says)[0]).text);
  }
  return s;
}

constexpr std::string_view name_of(Reason r) {
  std::string_view s;
  template for (constexpr auto e : reason_enumerators()) {
    if (r == [:e:]) s = std::define_static_string(std::meta::identifier_of(e));
  }
  return s;
}

}  // namespace ycxx::interp
