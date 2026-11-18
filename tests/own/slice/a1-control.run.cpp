// α1 (#39): if/else, while, do, for, break, continue, ?:, && and || short-circuit ([stmt], [expr.log.and]).
// EXIT: 0
int calls = 0;
bool bump() { ++calls; return true; }

int main() {
  int sum = 0;
  for (int i = 0; i < 10; ++i) {
    if (i == 3) continue;
    if (i == 8) break;
    sum += i;
  }
  if (sum != 25) return 1;
  int n = 0;
  while (n < 5) n++;
  do { n += 10; } while (n < 30);
  if (n != 35) return 2;
  int m = n > 30 ? 1 : 2;
  if (m != 1) return 3;
  if (false && bump()) return 4;
  if (true || bump()) {} else return 5;
  if (calls != 0) return 6;
  int x = 5;
  x *= 3; x -= 1; x /= 2; x %= 4; x <<= 3; x >>= 1; x |= 1; x &= 7; x ^= 2;
  if (x != 7) return 7;
  return 0;
}
