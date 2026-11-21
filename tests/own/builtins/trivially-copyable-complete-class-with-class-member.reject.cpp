// #218: The complete containing class has an unproved class-valued subobject in the current implementation.
// XFAIL: clang,gcc Both peers support this valid complete operand beyond the current ycxx classifier scope.
struct Child { int payload; };
struct Value { Child child; };
constexpr bool observed = __is_trivially_copyable(Value); //! error
