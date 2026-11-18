// α1 (#39): integer arithmetic, precedence and the usual arithmetic conversions ([expr.arith.conv]).
// EXIT: 0
int main() {
  int a = 7, b = 3;
  long l = 1000000000L * 4;
  unsigned u = 0u - 1u;            // wraps ([basic.fundamental]/2)
  if (a + b * 2 != 13) return 1;
  if ((a + b) * 2 != 20) return 2;
  if (a / b != 2 || a % b != 1) return 3;
  if (-a / b != -2 || -a % b != -1) return 4;   // truncation toward zero ([expr.mul]/4)
  if ((a << 4) != 112 || (a >> 1) != 3) return 5;
  if ((a & b) != 3 || (a | b) != 7 || (a ^ b) != 4 || ~a != -8) return 6;
  if (l != 4000000000L) return 7;
  if (u != 4294967295u) return 8;
  if (!(a > b) || a < b || a <= b || !(a >= b) || a == b) return 9;
  char c = 'A';
  if (c + 1 != 66) return 10;      // integral promotion ([conv.prom])
  bool t = a > b;
  if (t + t != 2) return 11;
  return 0;
}
