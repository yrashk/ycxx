// #1178: hidden friends for every unary operator and compound assignment, on classes.
// EXIT: 0
namespace n {
class V {
  int v;
 public:
  constexpr V(int x) : v(x) {}
  constexpr int get() const { return v; }
  friend constexpr int operator+(const V& x) { return x.v + 1; }
  friend constexpr int operator~(const V& x) { return x.v + 2; }
  friend constexpr bool operator!(const V& x) { return x.v == 3; }
  friend constexpr int& operator*(V& x) { return x.v; }
  friend constexpr int operator&(const V& x) { return x.v + 4; }
  friend constexpr V& operator++(V& x) { x.v += 10; return x; }
  friend constexpr V& operator--(V& x) { x.v -= 10; return x; }
  friend constexpr V& operator+=(V& x, V y) { x.v += y.v; return x; }
  friend constexpr V& operator-=(V& x, V y) { x.v -= y.v; return x; }
  friend constexpr V& operator*=(V& x, V y) { x.v *= y.v; return x; }
  friend constexpr V& operator/=(V& x, V y) { x.v /= y.v; return x; }
  friend constexpr V& operator%=(V& x, V y) { x.v %= y.v; return x; }
  friend constexpr V& operator<<=(V& x, int y) { x.v <<= y; return x; }
  friend constexpr V& operator>>=(V& x, int y) { x.v >>= y; return x; }
  friend constexpr V& operator&=(V& x, V y) { x.v &= y.v; return x; }
  friend constexpr V& operator^=(V& x, V y) { x.v ^= y.v; return x; }
  friend constexpr V& operator|=(V& x, V y) { x.v |= y.v; return x; }
  friend constexpr int& operator+=(int& x, V y) { return x += 100 + y.v; }
};
}
constexpr int run() {
  n::V v(3);
  if (+v != 4) return 1;
  if (~v != 5) return 2;
  if (!!v) return 3;
  if (&v != 7) return 4;
  *v = 5;
  if (v.get() != 5) return 5;
  if ((++v).get() != 15) return 6;
  if ((--v).get() != 5) return 7;
  ((v += 3) -= 1) *= n::V(6);
  if (v.get() != 42) return 8;
  v /= 4; if (v.get() != 10) return 9;
  v %= 7; if (v.get() != 3) return 10;
  v <<= 4; if (v.get() != 48) return 11;
  v >>= 2; if (v.get() != 12) return 12;
  v &= 9; if (v.get() != 8) return 13;
  v ^= 3; if (v.get() != 11) return 14;
  v |= 4; if (v.get() != 15) return 15;
  int i = 1;
  i += v;
  if (i != 116) return 16;
  return 0;
}
static_assert(run() == 0);
int main() { return run(); }
