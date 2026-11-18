// α2 (#39): a namespace-scope variable with a constant initializer, a loop, and calls into libSystem.
// STDOUT: 0123456789
// STDOUT: done
extern "C" int putchar(int);
extern "C" int puts(const char*);

int count = 10;

int main() {
  for (int i = 0; i < count; ++i) putchar('0' + i);
  putchar('\n');
  const char* msg = "done";
  puts(msg);
  return 0;
}
