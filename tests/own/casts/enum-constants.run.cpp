// #133, from the step's review: casts of constants to enumerations, also between enumerations,
// are constants: in static_assert, array bounds, enumerators and constexpr variables
// ([expr.static.cast]/10, [expr.const]).
// EXIT: 0
enum Uf { a1 = 1, a2 = 2 };
enum class Sc : unsigned char { z = 200 };
static_assert((Uf)2 == 2);
static_assert(Uf(2));
static_assert(static_cast<Uf>(2) == a2);
constexpr int i = (int)(Uf)2;
int x[Uf(2)];
enum G { g0 = (Uf)1 };
int main() {
  Sc s = static_cast<Sc>(a1);
  Sc t = (Sc)456;  // the value of the underlying type: 200
  return i + g0 - 3 + (int)s - 1 + (t == Sc::z ? 0 : 1);
}
