// Using-declarations naming members of a dependent base in a partial specialization: the
// specialization chosen for the arguments brings in that base's member functions
// ([namespace.udecl]/3, /11, [temp.spec.partial], [temp.dep.type]). (#1150)
struct One {
  int f(int) { return 1; }
  int operator()(int) const { return 2; }
};
struct Two {
  int f(char) { return 3; }
  int operator()(char) const { return 4; }
};
template <class T, class U> struct W {
  int f(double) { return 9; }
};
template <class T> struct W<T, T*> : T {
  using T::f;
  int f(double) { return 10; }
  using T::operator();
};
template <class T> struct W<T, const T> : T {
  using T::operator();
  int operator()(int) const { return 20; }   // hides One::operator()(int)
};
int main() {
  W<One, One*> a;
  W<Two, Two*> b;
  W<One, const One> c;
  W<One, int> d;
  int bad = 0;
  if (a.f(1) != 1 || a.f(1.5) != 10 || a(1) != 2) bad |= 1;
  if (b.f('c') != 3 || b.f(1.5) != 10 || b('c') != 4) bad |= 2;
  if (c(1) != 20) bad |= 4;
  if (d.f(1) != 9) bad |= 8;
  return bad;
}
