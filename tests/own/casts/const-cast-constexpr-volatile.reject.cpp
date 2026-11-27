// XFAIL: gcc GCC 16 allows access through a cast to a non-volatile glvalue in constant evaluation.
// #1424, [dcl.type.cv], [expr.const.core]: the object remains volatile when a cast changes the view.
constexpr int read_volatile() {
  volatile int value = 1;
  return const_cast<int&>(value);
}
static_assert(read_volatile() == 1); //! error

constexpr bool write_volatile() {
  volatile int value = 1;
  *const_cast<int*>(&value) = 2;
  return true;
}
static_assert(write_volatile()); //! error
