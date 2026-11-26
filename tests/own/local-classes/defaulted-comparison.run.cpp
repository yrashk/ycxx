// Local classes (#117), spec-only: a local class may declare defaulted equality as a member, and
// overload operators with member functions defined in the class ([class.compare.default],
// [over.oper], [class.local]).
int main() {
  struct P {
    int x;
    int y;
    bool operator==(const P&) const = default;
    P operator+(const P& o) const { return {x + o.x, y + o.y}; }
    int operator()(int k) const { return x * k + y; }
  };
  P a{1, 2}, b{1, 2}, c{3, 4};
  if (!(a == b)) return 1;
  if (a == c) return 2;
  if (!(a != c)) return 3;
  P s = a + c;
  if (!(s == P{4, 6})) return 4;
  if (s(10) != 46) return 5;
  return 0;
}
