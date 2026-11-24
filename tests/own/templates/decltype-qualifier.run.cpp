// A decltype-specifier before `::` ([expr.prim.id.qual], [dcl.type.decltype]), dependent or not:
// a member type after `typename` or where a type is assumed ([temp.res.general]), and a member. (#1243)
// STDOUT: 2 8 9 6
// STDOUT: 405 37 5 16
// STDOUT: 17 6
// STDOUT: 8 9 3 10 3 12 2 67
// EXIT: 0
extern "C" int printf(const char*, ...);

template <class T> struct ti {
  using type = T;
  template <class U> using with = ti<U>;
  static constexpr int size = sizeof(T);
  static int call(int x) { return x + int(sizeof(T)); }
};
template <class T> ti<T> p();

// A deduced return type, unknown until the arguments are.
template <class T> consteval auto select() {
  if constexpr (sizeof(T) == 1) return ti<int>{};
  else return ti<T>{};
}

// In a class template's members.
template <class T> struct P {
  using type = typename decltype(p<T>())::type;
  using assumed = decltype(p<T>())::type;
  using selected = typename decltype(select<T>())::type;
  using nested = typename decltype(select<T>())::template with<char>::type;
  typename decltype(select<T>())::type field = 3;
  decltype(select<T>())::type other = 4;
  typename decltype(select<T>())::type get() const { return field; }
  int set(typename decltype(select<T>())::type v) { field = v; return int(sizeof(v)); }
  static constexpr int size = decltype(select<T>())::size;
};
P<int>::type namespace_scope = 2;

// A pack, through a static member function template with a deduced return type.
template <class... Ts> struct Greatest {
  template <class C, class... Rest> static consteval auto pick() {
    if constexpr (((sizeof(C) >= sizeof(Ts)) && ...)) return ti<C>{};
    else return pick<Rest...>();
  }
  using type = typename decltype(pick<Ts...>())::type;
};
// A static member function without a head.
template <class T> struct Member {
  static consteval auto pick() {
    if constexpr (__is_same(T, char)) return ti<long>{};
    else return ti<T>{};
  }
  using type = typename decltype(pick())::type;
};

// In function templates: locals, expressions, casts, calls, and what looks like a declaration.
template <class T> int body(T x) {
  typename decltype(select<T>())::type local = x;
  const typename decltype(p<T>())::type also = x;
  using alias = typename decltype(select<T>())::type;
  int n = decltype(select<T>())::call(1);
  decltype(select<T>())::call(2);
  n += int(typename decltype(select<T>())::type(x));
  int initialized(decltype(select<T>())::size);
  auto with = [&](typename decltype(select<T>())::type y) { return int(y) + int(sizeof(y)); };
  return n + int(local) + int(also) + int(sizeof(alias)) + initialized + with(x);
}
template <class T> int arguments() {
  return ti<typename decltype(p<T>())::type>::size + int(sizeof(typename decltype(select<T>())::type));
}

// Alias templates, named with known and with dependent arguments.
template <class T> using declared_t = typename decltype(p<T>())::type;
template <class T> using selected_t = typename decltype(select<T>())::type;
template <class T> using selection = decltype(select<T>());
template <class T> struct Uses {
  selected_t<T> m = 6;
  using t = typename selection<T>::type;
};
template <class T> int through_alias() {
  selected_t<T> v = 9;
  return int(v) + int(sizeof(v)) + selection<T>::size;
}

// Outside templates.
ti<short> object;
decltype(object)::type variable = 8;
typename decltype(object)::type with_typename = 9;
using Outside = decltype(p<char>())::type;
decltype(p<int>())::type function(decltype(object)::type x) { return x + decltype(object)::size; }
namespace N { enum class E { a = 1, b = 5 }; }
N::E enumeration();

template <class A, class B> constexpr bool same = __is_same(A, B);
static_assert(same<P<char>::type, char> && same<P<char>::assumed, char> && same<P<char>::selected, int>);
static_assert(same<P<long>::selected, long> && same<P<int>::nested, char> && P<char>::size == sizeof(int));
static_assert(same<Greatest<char, long, int>::type, long> && same<Greatest<char>::type, char>);
static_assert(same<Member<char>::type, long> && same<Member<int>::type, int>);
static_assert(same<declared_t<char>, char> && same<selected_t<char>, int> && same<selection<char>, ti<int>>);
static_assert(same<Uses<char>::t, int> && sizeof(Uses<char>) == sizeof(int));
static_assert(same<Outside, char> && decltype(object)::size == sizeof(short));
static_assert(int(decltype(enumeration())::b) == 5);

int main() {
  P<char> c;
  P<short> s;
  int set = c.set(4) + s.set(5);
  decltype(object)::type local = 10;
  int expression = decltype(object)::call(local);
  decltype(object)::call(1);
  int value(decltype(object)::size);
  int conversion = int(sizeof(decltype(object)::type)) + int(decltype(p<char>())::type(65));
  printf("%d %d %d %d\n", namespace_scope, c.get() + c.other, s.get() + s.other, set);
  printf("%d %d %d %d\n", body('a'), body(1L), arguments<char>(), arguments<long>());
  printf("%d %d\n", through_alias<char>(), Uses<char>{}.m);
  printf("%d %d %d %d %d %d %d %d\n", variable, with_typename, function(1), local, 3, expression, value, conversion);
}
