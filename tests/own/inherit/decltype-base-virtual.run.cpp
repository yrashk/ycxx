// A decltype-specifier as a base that is polymorphic ([class.derived.general], [class.virtual]), and
// virtual functions and lambdas in the members of a class with such a base. (#1243, #1153, #338)
// STDOUT: 1 24 31 8 102 4 6 9 16 44 21 3
// EXIT: 0
extern "C" int printf(const char*, ...);

int destroyed = 0;
struct Root {
  virtual ~Root() { ++destroyed; }
  virtual int id() { return 1; }
  virtual int size() = 0;
};
template <class T> struct Sized : Root {
  int size() override { return int(sizeof(T)); }
  T value{};
};
struct Plain {
  int size() { return 100; }
};
template <class T> Sized<T> sized();

// A declared return type; the class overrides nothing.
template <class T> struct Inherits : decltype(sized<T>()) {};

// It overrides a member of the base, calls that member, and adds one of its own.
template <class T> struct Overrides : decltype(sized<T>()) {
  int id() override { return 20 + this->size(); }
  virtual int extra() { return Sized<T>::id() + 30; }
};

// Deduced: a polymorphic base for some arguments, not for others.
template <class T> consteval auto select() {
  if constexpr (sizeof(T) == 1) return Plain{};
  else return Sized<T>{};
}
template <class T> struct Selected : decltype(select<T>()) {
  int size() { return (sizeof(T) == 1 ? 2 : 0) + decltype(select<T>())::size(); }
};

// A member type of a decltype-specifier, with the class's own overrider final.
template <class T> struct Box { using type = Overrides<T>; };
template <class T> Box<T> box();
template <class T> struct Nested : decltype(box<T>())::type {
  int size() final { return 4; }
  ~Nested() override = default;
};

// Members of a class with such a base: lambdas in a static data member's initializer, in a static
// member function, and in an overrider.
template <class T> struct Lambdas : decltype(sized<T>()) {
  static constexpr auto twice = [](int n) { return 2 * n; };
  static inline int count = [] { return int(sizeof(T)) + 5; }();
  static auto pick() { return [](int n) { return n + int(sizeof(T)) + 12; }; }
  int id() override { return [this] { return 40 + this->size(); }(); }
};

// Its own static member with a deduced return type in a member declaration, beside virtual ones.
template <class T> struct Own : decltype(sized<T>()) {
  static auto make() { return Box<T>{}; }
  using Made = typename decltype(make())::type;
  virtual int made() { return Made().id() - 3; }
};

int id(Root& r) { return r.id(); }
int size(Root& r) { return r.size(); }

int main() {
  Inherits<int> i;
  Overrides<int> o;
  Selected<long> s;
  Selected<char> c;
  Nested<int> n;
  Lambdas<int> l;
  Own<int> w;
  static_assert(sizeof(Selected<char>) == 1 && Lambdas<char>::twice(4) == 8);
  printf("%d %d %d %d %d %d %d %d %d %d %d", id(i), id(o), o.extra(), size(s), c.size(), size(n), l.twice(3), Lambdas<int>::count,
         Lambdas<char>::pick()(3), id(l), w.made());
  destroyed = 0;
  {
    Nested<short> ns;
    Lambdas<short> ls;
    Inherits<char> ic;
  }
  printf(" %d\n", destroyed);
}
