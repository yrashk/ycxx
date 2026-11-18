// δ s4 (#86), from the enumerations' review: values beyond int choose a wider underlying type
// ([dcl.enum]/7), unsigned int where it holds them; a fixed underlying type is the better
// promotion ([conv.prom]/4, [over.ics.rank]); inside the list an enumerator has its value's type
// ([dcl.enum]/5); a nested enumeration as a parameter type; an enumeration variable at namespace
// scope.
// EXIT: 0
enum Big { b0 = 0x100000000LL };
enum U { u0 = 0x80000000u };
enum E : short { e1 = 1 };
enum class C { a = 1, b = a + 1 };
struct S { enum Mode { x, y }; };
enum G { g3 = 3 };
G global = g3;
int f(short) { return 0; }
int f(int) { return 1; }
int m(S::Mode v) { return v; }
int main() {
  long long x = b0;
  if (x != 0x100000000LL) return 1;
  long long y = -u0;
  if (y != 0x80000000LL) return 2;
  if (f(e1) != 0) return 3;
  if (C::b == C::a) return 4;
  if (m(S::y) != 1) return 5;
  if (global != g3) return 6;
  return 0;
}
