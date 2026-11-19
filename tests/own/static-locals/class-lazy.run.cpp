// ι (#291): a static block variable of class type with a user-provided constructor is
// constructed the first time control passes through its declaration ([stmt.dcl]/3): not
// before main, and never if its function is never called. It is destroyed after main returns
// ([basic.start.term]/1); one that was never constructed is never destroyed.
// EXIT: 0
// STDOUT: main
// STDOUT: +a
// STDOUT: used 1
// STDOUT: used 2
// STDOUT: end
// STDOUT: -a
extern "C" int putchar(int);

void say(const char* s) {
  while (*s) putchar(*s++);
}

void line(const char* s) {
  say(s);
  putchar('\n');
}

struct Noisy {
  char name;
  int uses;
  Noisy(char n) : name(n), uses(0) {
    putchar('+');
    putchar(name);
    putchar('\n');
  }
  ~Noisy() {
    putchar('-');
    putchar(name);
    putchar('\n');
  }
};

int use_a() {
  static Noisy a('a');
  return ++a.uses;
}

int never_called() {
  static Noisy z('z');
  return z.uses;
}

int main() {
  line("main");
  int u = use_a();
  say("used ");
  putchar('0' + u);
  putchar('\n');
  u = use_a();
  say("used ");
  putchar('0' + u);
  putchar('\n');
  line("end");
  if (u != 2) return 1;
  return 0;
}
