// An explicit specialization of a class template has a base-clause like any class
// ([temp.expl.spec], [class.derived.general]) (#1134).
// EXIT: 0
using size_t = decltype(sizeof 0);
template<class T, T V> struct integral_constant { using value_type = T; static constexpr T value = V; };

struct B { int u = 1; };
struct C { static constexpr int w = 5; };
template<class T> struct P;
template<> struct P<int> : B {};
template<> struct P<long> : B, C { int own = 3; };
template<class S, class U> struct sign_pair_def { using s = S; using u = U; };
template<> struct P<short> : sign_pair_def<short, unsigned short> {};

template<class T> struct Q { static constexpr int id = 7; };
template<> struct Q<int> : Q<char> {};        // a base that is another specialization of Q

template<class T, class U> struct pair {};
template<class T> struct tuple_size;
template<> struct tuple_size<pair<int, int>> : integral_constant<size_t, 2> {};

static_assert(P<long>::w == 5);
static_assert(Q<int>::id == 7);
static_assert(__is_same(P<short>::u, unsigned short));        // an inherited member type
static_assert(__is_same(tuple_size<pair<int, int>>::value_type, size_t));
static_assert(tuple_size<pair<int, int>>::value == 2);

int base_u(const B& b) { return b.u; }

int main() {
  P<int> p;
  B& b = p;                                  // derived-to-base conversion
  P<long> l;
  return base_u(p) == 1 && b.u == 1 && sizeof(P<int>::u) == sizeof(int) && P<int>{}.u == 1 && l.u + l.own == 4 &&
                 static_cast<const C&>(l).w == 5 ? 0 : 1;
}
