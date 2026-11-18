// α3 (#85): a program that includes a small local header ([cpp.include]) and uses its object-like
// macros ([cpp.replace.general]), one of which uses a macro the includer defines first.
// STDOUT: hello from a header
// EXIT: 42
#define FACTOR 7
#include "inputs/greet.h"
#include "inputs/greet.h"

int scale(int x) { return x * FACTOR; }

int main() {
  puts(GREETING);
  if (scale(2) != 14) return 1;
  return ANSWER;
}
