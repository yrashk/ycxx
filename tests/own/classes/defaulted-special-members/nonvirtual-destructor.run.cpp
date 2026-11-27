// #157 [class.dtor]: a defaulted destructor that is not virtual, in a class with virtual functions,
// is defined once and destroys its subobjects, in ordinary classes and class templates.
// EXIT: 0
int ends = 0;
struct N {
  int v;
  N(int x = 1) noexcept : v(x) {}
  N(const N& o) noexcept : v(o.v) {}
  ~N() { ++ends; }
};

struct Ordinary { N n; ~Ordinary() = default; virtual int f() { return n.v; } };
struct Root { virtual int f() { return 1; } };
struct Derived : Root { N n; ~Derived() = default; };
struct Polymorphic { N n; virtual int f() { return n.v; } ~Polymorphic() = default; Polymorphic() noexcept = default; };
struct FromPolymorphic : Polymorphic { N m; ~FromPolymorphic() = default; int f() override { return m.v + 1; } };
template <class T> struct Templated : Root { T n; Templated() noexcept = default; Templated(const Templated&) noexcept = default; ~Templated() = default; };
template <class T> struct Base { T t; virtual int f() { return t.v; } Base() noexcept = default; };
template <class T> struct FromBase : Base<T> { FromBase() noexcept = default; FromBase(const FromBase&) noexcept = default; ~FromBase() = default; };
struct Holder { Templated<N> t; ~Holder() = default; };

template <class T> int count() noexcept {
  ends = 0;
  { T x; }
  return ends;
}

int main() {
  if (count<Ordinary>() != 1) return 1;
  if (count<Derived>() != 1) return 2;
  if (count<FromPolymorphic>() != 2) return 3;
  if (count<Templated<N>>() != 1) return 4;
  if (count<FromBase<N>>() != 1) return 5;
  if (count<Holder>() != 1) return 6;
  FromPolymorphic p;
  Polymorphic& base = p;
  return base.f() == 2 ? 0 : 7;
}
