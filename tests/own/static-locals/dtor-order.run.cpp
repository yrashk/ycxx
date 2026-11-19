// ι (#291): static block variables are destroyed after main returns in the reverse order of the
// completion of their constructors ([basic.start.term]/1), whatever the order of the
// functions in the source. A static constructed inside another static's constructor completes
// first, so it is destroyed after it.
// EXIT: 0
// STDOUT: +b
// STDOUT: +c
// STDOUT: +a
// STDOUT: +o
// STDOUT: +i
// STDOUT: end
// STDOUT: -o
// STDOUT: -i
// STDOUT: -a
// STDOUT: -c
// STDOUT: -b
extern "C" int putchar(int);

struct Noisy {
  char name;
  Noisy(char n) : name(n) {
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

Noisy* a() {
  static Noisy x('a');
  return &x;
}

Noisy* b() {
  static Noisy x('b');
  return &x;
}

Noisy* c() {
  static Noisy x('c');
  return &x;
}

Noisy* inner() {
  static Noisy x('i');
  return &x;
}

struct Outer {
  Noisy n;
  // n's constructor runs, then inner()'s static is constructed and completes, then this
  // constructor completes.
  Outer() : n('o') { inner(); }
  ~Outer() {}
};

Outer* outer() {
  static Outer x;
  return &x;
}

int main() {
  b();
  c();
  a();
  b();
  a();
  outer();
  outer();
  putchar('e');
  putchar('n');
  putchar('d');
  putchar('\n');
  return 0;
}
