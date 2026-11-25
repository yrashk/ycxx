// #1153 [temp.inst], [temp.expl.spec]: instantiating a dynamic specialization inside a class does
// not look past it: specializations declared later, and naming the class, are theirs.
// EXIT: 0

template <class T> struct S {
  virtual int f() { return 1; }
  T v;
};
template <> struct S<void> {
  int z;
};

struct D : S<int> {
  int f() override { return 4; }
};
template <> struct S<D> {
  virtual int f() { return 6; }
};
template <> struct S<D*> {
  int pointer = 7;
};
template <> int S<D**>::f() { return 8; }

struct E {
  S<int> s;
  enum K { a };
};
template <> struct S<E::K> {
  int z = 9;
};

int main() {
  S<D> x;
  S<D>& y = x;
  S<D*> p;
  S<E::K> k;
  S<D**> m;
  S<D**>& n = m;
  return y.f() == 6 && p.pointer == 7 && n.f() == 8 && k.z == 9 ? 0 : 1;
}
