// ι (#291): static block variables and namespace-scope objects with static storage duration are
// destroyed in one sequence, the reverse of the order in which their initializations completed
// ([basic.start.term]/1). Namespace-scope variables of one translation unit are initialized in
// order of definition ([basic.start.dynamic], ordered initialization); a static block variable
// first reached from the constructor of the second one completes between the two. That the
// namespace-scope objects are initialized before main's first statement is
// implementation-defined ([basic.start.dynamic]); the Itanium ABI targets do so.
// EXIT: 0
// STDOUT: +G
// STDOUT: +l
// STDOUT: +H
// STDOUT: main
// STDOUT: +m
// STDOUT: end
// STDOUT: -m
// STDOUT: -H
// STDOUT: -l
// STDOUT: -G
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

Noisy* local_l() {
  static Noisy x('l');
  return &x;
}

Noisy* local_m() {
  static Noisy x('m');
  return &x;
}

struct Holder {
  Noisy n;
  Holder(char c) : n(c) {}
};

struct UsesLocal {
  Noisy n;
  UsesLocal(char c) : n((local_l(), c)) {}
};

Holder g('G');
UsesLocal h('H');

int main() {
  putchar('m');
  putchar('a');
  putchar('i');
  putchar('n');
  putchar('\n');
  local_m();
  local_l();  // already constructed: nothing printed
  local_m();
  putchar('e');
  putchar('n');
  putchar('d');
  putchar('\n');
  return 0;
}
