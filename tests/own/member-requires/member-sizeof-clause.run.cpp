// #1417 review [expr.prim.id.general]/3, [expr.sizeof]: an unevaluated operand of a member's
// requires-clause can name a non-static data member without an object.
// XFAIL: ycxx a non-static member named without an object in an unevaluated operand (#1299)
template<class T> struct S {
  T member;
  int e() requires (sizeof(member) == 4) { return 1; }
  int e() requires (sizeof(member) != 4) { return 2; }
};
int main() { return S<int>{}.e() == 1 && S<char>{}.e() == 2 ? 0 : 1; }
