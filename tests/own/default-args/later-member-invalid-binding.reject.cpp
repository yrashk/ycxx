// #354: lookup in a default argument sees the complete class, so the later member
// hides the namespace-scope variable. A default argument cannot use that member
// without an object ([class.mem.general], [basic.lookup.unqual], [dcl.fct.default]).
int value = 42;

struct S {
  int get(int n = value) { return n; } //! error
  int value;
};
