// C0 declarations (#39, #41): function declarations and definitions with parameters ([dcl.fct],
// [dcl.fct.def.general]), extern "C" ([dcl.link]), namespace-scope and local variables with
// initializers ([dcl.init]), and redeclarations ([basic.link]).
extern "C" int puts(const char*);
extern "C" {
int putchar(int);
}
int square(int);
int square(int x) { return x * x; }
int square(int);
long twice(long v);
long twice(long v) { return v + v; }
void nothing() {}
void nothing_either(void) { return; }
int unnamed_parameters(int, char, bool) { return 0; }
int g = 7;
unsigned u = 3u;
long big = 1L << 40;
char letter = 'a';
bool flag = true;
const int limit = 10;
const int twice_limit = limit * 2;
int a = 1, b = 2, c;
extern int declared_elsewhere;
static int file_local = 4;
static int helper() { return file_local; }
const char* greeting = "hello";
int array_bound[limit];
int main() {
  int x = 1;
  int y = x + 1, z;
  z = y;
  const long w = z;
  unsigned char uc = 200;
  signed char sc = -1;
  short s = 2;
  unsigned short us = 3;
  long long ll = 4;
  unsigned long long ull = 5;
  return square(x) + helper() + (twice(w) > 0) + uc + sc + s + us + ll + ull - a - b + c + g;
}
