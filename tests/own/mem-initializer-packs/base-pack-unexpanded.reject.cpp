// #1400: a base-class pack named as a mem-initializer-id without `...` ([temp.variadic]: a pack
// outside every pack expansion).
// XFAIL: ycxx needs a pack expansion in a class template's base-specifier-list (#308)
template<class... Bs> struct D : Bs... {
  D(int x) : Bs(x) {} //! error
};
int main() { return 0; }
