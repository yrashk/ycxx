// #1567, #1177: in a class template, its injected-class-name followed by `<` is the template's
// name ([temp.local]/1), so `A<U>` names the specialization `A<U>`. The class's constructors share
// its name but are not found by lookup ([class.ctor]/1) and do not hide it. Without arguments the
// name is still the current specialization. Each check agrees with Clang and GCC.
// EXIT: 0
template <class T> struct A {
  T v{};
  A() = default;
  A(T x) : v(x) {}
  // A converting constructor template, as allocator, pair and optional declare.
  template <class U> A(const A<U>& o) noexcept : v(static_cast<T>(o.v + 1)) {}
  // A member function template's parameter, by reference and by value.
  template <class U> int add(const A<U>& o) const { return static_cast<int>(v + o.v); }
  template <class U> int add_value(A<U> o) const { return static_cast<int>(v + o.v) * 10; }
  // A return type.
  template <class U> A<U> to() const { return A<U>(static_cast<U>(v)); }
  A<T> same() const { return *this; }
  // A qualifier, in a body and in a return type.
  using value_type = T;
  static constexpr int tag = sizeof(T);
  template <class U> static int tag_of() { return A<U>::tag; }
  template <class U> typename A<U>::value_type cast() const { return static_cast<typename A<U>::value_type>(v); }
  // Plain `A` is the current specialization: `A<T>`.
  A twice() const { return A(v * 2); }
  static constexpr bool plain_is_current = __is_same(A, A<T>);
  // A local declaration hides it: `A < 2` compares.
  static bool local(int A) { return A < 2; }
};

// Out of the class: a constructor template's and a member template's definitions.
template <class T> struct B {
  T v{};
  B() = default;
  template <class U> B(const B<U>&);
  template <class U> bool less(const B<U>& o) const;
};
template <class T> template <class U> B<T>::B(const B<U>& o) : v(static_cast<T>(o.v + 10)) {}
template <class T> template <class U> bool B<T>::less(const B<U>& o) const { return v < o.v; }

// A partial specialization's injected-class-name names the primary template ([temp.local]/1).
template <class T> struct P { T v; };
template <class T> struct P<T*> {
  T* p = nullptr;
  P() = default;
  template <class U> P(const P<U*>& o) : p(o.p) {}
};

// A non-static data member named as its class, which declares no constructor, hides the
// injected-class-name ([class.mem.general]): `D < 2` compares the member.
template <class T> struct D {
  int D;
  bool small() const { return D < 2; }
};

int main() {
  A<int> a(3);
  A<long> b(a);
  if (b.v != 4) return 1;
  if (a.add(b) != 7) return 2;
  if (a.add_value(b) != 70) return 3;
  A<char> c = a.to<char>();
  if (c.v != 3) return 4;
  if (a.same().v != 3) return 5;
  if (A<int>::tag_of<long long>() != 8) return 6;
  if (a.cast<double>() != 3.0) return 7;
  if (a.twice().v != 6) return 8;
  if (!A<int>::plain_is_current || !A<short>::plain_is_current) return 9;
  if (!A<int>::local(1) || A<int>::local(5)) return 10;
  B<int> x;
  x.v = 1;
  B<long> y(x);
  if (y.v != 11 || !x.less(y)) return 11;
  int i = 0;
  P<int*> p;
  p.p = &i;
  P<const int*> q(p);
  if (q.p != &i) return 12;
  ::D<int> d;
  d.D = 1;
  if (!d.small()) return 13;
  return 0;
}
