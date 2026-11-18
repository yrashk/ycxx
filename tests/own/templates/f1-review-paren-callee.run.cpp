// ζ f1 (#88), from the step's review: a parenthesized function template name, and a parenthesized
// template-id, are called like the name ([expr.prim.paren], [temp.names]); no argument-dependent
// lookup applies ([basic.lookup.argdep]/1).
// EXIT: 0
template<class T> int f(T) { return 1; }
int main() {
  if ((f)(1) != 1) return 1;
  if ((f<long>)(2) != 1) return 2;
  return 0;
}
