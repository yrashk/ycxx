// A decltype-specifier as a base ([class.derived.general], [dcl.type.decltype]): dependent in a
// class template, whose base is known once the template is instantiated. (#1243)
// STDOUT: 7 7 5 11 7 7 13 17 7 7 7 7
// EXIT: 0
extern "C" int printf(const char*, ...);

template <class T> struct S {
  T value = T(7);
  static constexpr int size = sizeof(T);
  int get() const { return int(value); }
};
template <class T> S<T> declared();

// A declared return type.
template <class T> class Declared : public decltype(declared<T>()) {};

// A deduced return type, unknown until the arguments are.
struct Wide { static constexpr int tag = 1; };
struct Narrow { static constexpr int tag = 2; };
template <class T> struct Other { static constexpr int tag = 3; using type = T; };
template <class T> consteval auto select() {
  if constexpr (__is_same(T, long)) return Wide{};
  else if constexpr (sizeof(T) == 1) return Narrow{};
  else return Other<T>{};
}
template <class T> class Selected : public decltype(select<T>()) {};
template <class T> class Selected<const T> : public Selected<T> {};
template <> class Selected<float> : public decltype(select<char>()) {};
// A member of the specialized template, and a partial specialization with a decltype base.
template <class T> class Selected<T*> : public decltype(select<T>()) {
public:
  static constexpr int pointer = 1;
};

// The access-specifier and its defaults.
template <class T> struct Public : decltype(declared<T>()) {};
template <class T> class Private : decltype(declared<T>()) {
public:
  int through() const { return this->get(); }
};

// A member type of a decltype-specifier.
template <class T> struct Box { using type = S<T>; };
template <class T> Box<T> box();
template <class T> struct Nested : decltype(box<T>())::type {};

// A cv-qualified class type; its cv-qualifiers are ignored.
template <class T> inline constexpr S<T> constant{};
template <class T> struct FromConst : decltype(constant<T>) {};
constexpr S<int> plain_constant{};
struct PlainFromConst : decltype(plain_constant) {};

// Outside templates, and a nondependent operand in one.
struct Plain : decltype(declared<int>()) {};
template <class T> struct Nondependent : decltype(declared<short>()) { T extra; };

// A parameter pack named in the operand.
template <class T, class... Rest> auto first(T, Rest...) { return S<T>{}; }
template <class... Ts> struct First : decltype(first(Ts()...)) {};

int main() {
  Declared<int> d;
  Declared<char> c;
  static_assert(Declared<char>::size == 1 && sizeof(Declared<long>) == sizeof(long));
  static_assert(Selected<long>::tag == 1 && Selected<char>::tag == 2 && Selected<int>::tag == 3);
  static_assert(Selected<const long>::tag == 1 && Selected<float>::tag == 2);
  static_assert(Selected<char*>::tag == 2 && Selected<char*>::pointer == 1);
  Selected<short>::type s = 5;
  Public<int> p;
  p.value = 11;
  Private<int> q;
  Nested<int> n;
  FromConst<int> f;
  f.value = 13;
  PlainFromConst pf;
  pf.value = 17;
  Plain plain;
  Nondependent<int> nd{};
  First<int, char*> one;
  const S<int>& base = d;
  printf("%d %d %d %d %d %d %d %d %d %d %d %d\n", d.get(), c.get(), int(s), p.get(), q.through(), n.get(), f.get(), pf.get(),
         plain.get(), nd.get(), one.get(), base.get());
}
