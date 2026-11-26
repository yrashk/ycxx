// Using-declarations of conversion functions in class templates, from a nondependent base and
// from a dependent one ([namespace.udecl]/1, [class.conv.fct], [temp.dep.type]). (#1150)
// XFAIL: ycxx conversion functions of a class with a base are not considered (#339); an explicit
// conversion function in a class template (#715)
struct B {
  explicit operator bool() const { return true; }
};
struct Ints {
  operator bool() const { return true; }
};
struct Chars {
  operator bool() const { return false; }
};
template <class T> struct N : B {
  using B::operator bool;
};
template <class Fn> struct D : Fn {
  using Fn::operator bool;
};
int main() {
  N<int> a;
  D<Ints> i;
  D<Chars> c;
  int bad = 0;
  if (!a || !static_cast<bool>(N<long>{})) bad |= 1;
  if (!i || c) bad |= 2;
  return bad;
}
