// [over.oper], [expr.call], [basic.lookup.argdep]: an explicit operator-function name is
// resolved like an ordinary function name.
// EXIT: 0
namespace Outer::Numbers {
struct Number { int value; };
int operator+(const Number& n, int increment) { return n.value + increment; }
int operator+(const Number& n, long increment) { return n.value + static_cast<int>(increment) + 10; }
int operator-(const Number& n) { return -n.value; }
}

struct Global { int value; };
int operator-(const Global& n, int decrement) { return n.value - decrement; }
int operator,(const Global& n, int increment) { return n.value + increment; }

int main() {
  Outer::Numbers::Number a{40};
  Global b{44};
  if (Outer::Numbers::operator+(a, 2) != 42) return 1;
  if (::operator-(b, 2) != 42) return 2;
  if (operator+(a, 2L) != 52) return 3;
  if (operator-(a) != -40 || operator-(b, 2) != 42) return 4;
  if (::operator,(b, 2) != 46) return 5;
  return 0;
}
