// The value returned from main is the exit status ([basic.start.main]/5), and what is written
// to stdout is compared.
// EXIT: 3
// STDOUT: hello
// STDOUT: 6
#include <cstdio>

int main() {
  std::puts("hello");
  std::printf("%d\n", 1 + 2 + 3);
  return 3;
}
