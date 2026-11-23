// [dcl.mptr], [dcl.fct]
struct C {};
using R = int;
void check() {
  typename ::R ((C::*ptr)(int) const &) = nullptr;
  typename ::R ((C::*named)(typename ::R (arg)) const &) = nullptr;
}
