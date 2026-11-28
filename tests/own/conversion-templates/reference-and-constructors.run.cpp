// #340 item 3: a reference to const binds to the temporary a conversion function template
// returns ([dcl.init.ref]/5.4), and a class with only non-viable converting constructors is
// copy-initialized through the source's conversion function ([over.match.copy]/1).
// EXIT: 0
template <class T> struct R { T t; template <class U> constexpr operator R<U>() const { return {U(t)}; } };
struct S { template <class T> operator T() const { return T(3); } };
struct B { int v; B(int x) : v(x) {} };
struct M { operator B() const { return B(8); } };
int main() {
  R<int> a{1};
  const R<long>& r = a;
  S s;
  const int& i = s;
  M m;
  B b = m;
  return r.t + i + b.v == 12 ? 0 : 1;
}
