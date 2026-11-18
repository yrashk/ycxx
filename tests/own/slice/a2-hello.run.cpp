// α2 (#39): a string literal ([lex.string]) passed to a C function from libSystem.
// STDOUT: hello, world
extern "C" int puts(const char*);

int main() {
  puts("hello, world");
  return 0;
}
