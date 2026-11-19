// ι (#313): the left operand of an assignment is the target of an overload set on the right
// ([over.over], [expr.assign]).
// EXIT: 0
int f(int x) { return x * 2; }
int f(long x) { return static_cast<int>(x) * 3; }
int f(char) { return 5; }

int g(int x) { return x - 1; }

int main() {
  int (*p)(int) = g;
  int (*q)(long) = nullptr;
  int (*c)(char) = 0;
  if (p(10) != 9) return 1;
  p = f;
  if (p(10) != 20) return 2;
  q = &f;
  if (q(10) != 30) return 3;
  c = (f);
  if (c('a') != 5) return 4;
  p = g;
  if (p(10) != 9) return 5;
  p = &f;
  if (p(1) != 2) return 6;
  return 0;
}
