// Local classes (#117), spec-only: naming an enclosing automatic variable in an unevaluated
// operand (sizeof, decltype) from a local class is not an odr-use and is allowed
// ([basic.def.odr], [expr.context], [class.local]).
int main() {
  double big[5] = {};
  char c = 'a';
  struct S {
    static unsigned long bytes() { return sizeof(big); }
    unsigned long other() const { return sizeof c + sizeof(decltype(big[0])); }
    decltype(c) tag = 'z';
  };
  if (S::bytes() != 5 * sizeof(double)) return 1;
  S s;
  if (s.other() != 1 + sizeof(double)) return 2;
  if (s.tag != 'z') return 3;
  big[0] = c;
  return 0;
}
