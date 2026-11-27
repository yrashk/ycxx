// Local classes (#117), spec-only: a member function of a local class cannot odr-use an automatic
// variable of the enclosing function: the class scope intervenes, so the variable is not
// odr-usable there ([basic.def.odr], [class.local]).
int main() {
  int x = 1;
  struct L {
    int get() { return x; } //! error
  };
  return L{}.get();
}
