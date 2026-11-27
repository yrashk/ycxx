// A derived member template hides a using-declared base member template only if their
// template-heads are equivalent ([namespace.udecl]/11, [basic.scope.scope]/4, [temp.over.link]) (#1150).
// XFAIL: clang hides the base template by name and rejects d.f<5>()
struct B { template <int N> int f() { return N; } };
struct D : B { using B::f; template <class T> int f() { return 2; } };
int main() {
  D d;
  return d.f<5>() == 5 && d.f<int>() == 2 ? 0 : 1;
}
