// C0 expressions (#39, #41): every operator of the subset with its precedence and associativity
// ([expr.compound]), integral promotions and conversions ([conv.prom], [conv.integral]), the usual
// arithmetic conversions ([expr.arith.conv]), and value categories ([basic.lval]).
int f(int a, int b) { return a - b; }
long widen(long v) { return v; }
bool truth(int v) { return v; }
int main() {
  int a = 1 + 2 * 3 - 4 / 2 % 3;
  int b = (a << 2) >> 1 | a & 3 ^ 5;
  bool c = a < b && b <= 10 || a > b && !(a >= b) || a == b || a != b;
  int d = c ? a : b;
  int e = ~a + -b + +a;
  e = d = a;
  e += 1;
  e -= 2;
  e *= 3;
  e /= 2;
  e %= 7;
  e <<= 1;
  e >>= 1;
  e &= 15;
  e |= 16;
  e ^= 3;
  int g = e++ + ++e - e-- - --e;
  unsigned u = 4000000000u;
  long l = u + a;
  unsigned long ul = l * u;
  char ch = 'x';
  int promoted = ch + 1;
  bool flag = a;
  flag = ul;
  int from_bool = flag + true;
  int comma = (a, b, 3);
  (a < b ? a : b) = 4;
  ++(++a);
  a = f(f(1, 2), widen(3) > 2) + truth(a - a);
  return a + b + c + d + e + g + l + ch + promoted + from_bool + comma;
}
