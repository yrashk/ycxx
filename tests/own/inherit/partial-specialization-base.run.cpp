// A partial specialization of a class template has a base-clause; a base that depends on the
// partial specialization's own parameters is substituted with the deduced arguments
// ([temp.spec.partial], [class.derived.general], [temp.inst]) (#1134).
// EXIT: 0
using size_t = decltype(sizeof 0);
template<class T, T V> struct integral_constant { using value_type = T; static constexpr T value = V; };

struct B { int u = 1; };
template<class T> struct P { int u = 9; };
template<class T> struct P<T*> : B {};      // a nondependent base

template<class T> struct Box { T held{}; };
template<class T> struct P<T&> : Box<T> {}; // a dependent base

template<class T> struct X;
template<class T, size_t N> struct X<T[N]> : integral_constant<size_t, N> {};

template<class T, class U> struct pair {};
template<class T> struct tuple_size;
template<class T> struct tuple_size<pair<T, T>> : integral_constant<size_t, 2> {};

static_assert(X<int[3]>::value == 3);
static_assert(X<char[17]>::value == 17);
static_assert(__is_same(X<int[3]>::value_type, size_t));        // an inherited member type
static_assert(__is_same(decltype(P<int&>::held), int));
static_assert(tuple_size<pair<long, long>>::value == 2);

int base_u(const B& b) { return b.u; }
size_t width(const integral_constant<size_t, 3>& c) { return c.value; }

int main() {
  P<int*> p;
  B& b = p;                                   // derived-to-base conversion
  P<long&> r;
  Box<long>& box = r;
  box.held = 4;
  X<int[3]> x;
  return base_u(p) == 1 && b.u == 1 && P<int*>{}.u == 1 && sizeof(P<int*>::u) == sizeof(int) && P<int>{}.u == 9 && r.held == 4 &&
                 width(x) == 3 && X<int[3]>::value == 3 ? 0 : 1;
}
