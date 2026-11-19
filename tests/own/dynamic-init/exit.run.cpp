// ι (#94), spec-only: calling `exit` from main destroys the namespace-scope objects in reverse
// order of construction, as returning from main does, and the program's exit status is the
// argument ([basic.start.term], [basic.start.main], [support.start.term]).
// STDOUT: xyM
// STDOUT: YX
// EXIT: 3
extern "C" int putchar(int);
extern "C" void exit(int);

struct Guard {
  char c;
  Guard(char x) : c(x) { putchar(x); }
  ~Guard() {
    putchar(c - 'a' + 'A');
    if (c == 'x') putchar('\n');
  }
};

Guard x('x');
Guard y('y');

int main() {
  putchar('M');
  putchar('\n');
  exit(3);
  return 0;
}
