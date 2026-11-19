// #133: the C header <stdio.h> (the macOS SDK's, through libycxx's search path) compiles: its
// typedefs, unions, FILE with function-pointer members, attributes and asm labels; printf, puts,
// fputs and stdout work at run time.
// STDOUT: hello 42 world
// STDOUT: done
// STDOUT: via FILE
#include <stdio.h>

int main() {
  printf("hello %d %s\n", 42, "world");
  puts("done");
  FILE* f = stdout;
  fputs("via FILE\n", f);
  return 0;
}
