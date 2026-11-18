// Integer division truncates toward zero and % follows it ([expr.mul]/4); unsigned arithmetic
// wraps ([basic.fundamental]/2). Every compiler must print the same.
#include <cstdio>

int main() {
  int a = -7, b = 2;
  unsigned u = 0;
  std::printf("%d %d %u\n", a / b, a % b, u - 1);
}
