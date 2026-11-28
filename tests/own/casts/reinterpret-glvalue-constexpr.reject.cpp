// #199, [expr.const]: a reached glvalue reinterpret_cast prevents constant evaluation even when
// nothing is accessed through its result.
// XFAIL: gcc GCC 16 accepts a glvalue reinterpret_cast that is not used for an access (#1702)
int item;
constexpr const unsigned& view = reinterpret_cast<const unsigned&>(item); //! error
constexpr bool address_only() {
  int local = 1;
  unsigned& u = reinterpret_cast<unsigned&>(local);
  return &u != nullptr;
}
static_assert(address_only()); //! error
