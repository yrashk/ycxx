// #1424, [dcl.type.cv], [expr.const.core]: a cast cannot make a const object mutable.
constexpr bool write_const_object() {
  const int value = 1;
  *const_cast<int*>(&value) = 2; //! error[gcc]
  return true;
}
static_assert(write_const_object()); //! error

struct Record { int field; };
constexpr bool write_const_subobject() {
  const Record record{1};
  const_cast<int&>(record.field) = 2; //! error[gcc]
  return true;
}
static_assert(write_const_subobject()); //! error
