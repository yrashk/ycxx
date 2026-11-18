// α2 (#39, #41): string literals ([lex.string]) as arrays of const char, pointers to const char,
// indirection, subscripts and pointer arithmetic ([expr.unary.op], [expr.sub], [expr.add]), and
// calls to extern "C" functions ([dcl.link]).
extern "C" int puts(const char*);
extern "C" int putchar(int);
const char* message = "hello, world";
int length(const char* s) {
  int n = 0;
  while (*s++) ++n;
  return n;
}
void print(const char* s) {
  for (const char* p = s; *p != 0; p = p + 1) putchar(*p);
  putchar('\n');
}
int main() {
  puts("hello" " " "world");
  print(message);
  const char* p = message;
  char second = p[1];
  const char* end = p + length(p);
  long diff = end - p;
  bool same = p == message && end != 0 && p != nullptr;
  if (!p) return 1;
  p += 2;
  p -= 1;
  return second + diff + same + (p > message);
}
