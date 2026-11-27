// #1400: a mem-initializer pack expansion whose base pack and function parameter pack have
// different lengths ([temp.variadic]: all packs expanded by one expansion shall have the same
// number of elements).
// XFAIL: ycxx needs a pack expansion in a class template's base-specifier-list (#308)
struct B1 { B1(int) {} };
struct B2 { B2(int) {} };
template<class... Bs> struct D : Bs... {
  template<class... A> D(A... a) : Bs(a)... {} //! error
};
D<B1, B2> d(1, 2, 3);
int main() { return 0; }
