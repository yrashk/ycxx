// δ s2 (#86): constexpr member functions evaluated in constant expressions on constexpr objects
// and on objects created during constant evaluation ([dcl.constexpr], [expr.const],
// [class.mfct.non.static]).
// EXIT: 0
struct Fraction {
  int num;
  int den;

  constexpr bool proper() const { return num < den; }
  constexpr int whole() const { return num / den; }
  constexpr void add_whole(int n) { num += n * den; }
};

constexpr Fraction seven_thirds = {7, 3};
static_assert(seven_thirds.whole() == 2);
static_assert(!seven_thirds.proper());

constexpr int mixed() {
  Fraction f = {1, 4};
  f.add_whole(3);
  return f.num;
}
static_assert(mixed() == 13);

int main() {
  Fraction f = {5, 2};
  f.add_whole(1);
  if (f.num != 7 || f.whole() != 3) return 1;
  if (f.proper()) return 2;
  return 0;
}
