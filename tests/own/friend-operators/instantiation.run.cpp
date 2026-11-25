// #1178: dependent operands find hidden friends at instantiation ([temp.dep.candidate]).
// EXIT: 0
template <class T> constexpr T inv(T x) { return ~x; }
template <class T> constexpr T& set(T& x, T y) { return x |= y; }
template <class T> constexpr T& inc(T& x) { return ++x; }
template <class T, class U> concept can_or = requires(T& t, U u) { t |= u; };
struct Later {
  enum E : unsigned { a = 1, b = 2 };
  friend constexpr E operator~(E x) { return E(~unsigned(x) & 3u); }
  friend constexpr E& operator|=(E& x, E y) { return x = E(unsigned(x) | unsigned(y)); }
};
template <class T> struct Box {
  T v;
  friend constexpr Box operator~(const Box& b) { return {T(~b.v)}; }
  friend constexpr Box& operator|=(Box& b, const Box& o) { b.v |= o.v; return b; }
  friend constexpr Box& operator++(Box& b) { ++b.v; return b; }
  template <class U> friend constexpr Box& operator+=(Box& b, U u) { b.v += u; return b; }
};
static_assert(can_or<Later::E, Later::E> && !can_or<Later::E, int> && !can_or<Later, int>);
constexpr int run() {
  if (inv(Later::a) != Later::b) return 1;
  Later::E e = Later::a;
  if (set(e, Later::b) != 3u) return 2;
  Box<int> b{5};
  if (inv(b).v != -6) return 3;
  if (set(b, Box<int>{8}).v != 13) return 4;
  if (inc(b).v != 14) return 5;
  if ((b += 2).v != 16) return 6;
  return 0;
}
static_assert(run() == 0);
int main() { return run(); }
