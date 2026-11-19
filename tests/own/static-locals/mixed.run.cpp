// ι (#291): static block variables in functions of two objects, one compiled by the compiler
// under test and one by Clang (mixed-peer.cpp), each calling the other's: every static is
// initialized once, on first use, and keeps its state ([stmt.dcl]/3; the guard protocol of
// the Itanium C++ ABI §3.3.3), and the class-type ones from both objects are destroyed after
// main in reverse order of construction ([basic.start.term]/1).
// PEER: mixed-peer.cpp
// EXIT: 0
// STDOUT: +m
// STDOUT: +p
// STDOUT: end
// STDOUT: -p
// STDOUT: -m
extern "C" int putchar(int);

int peer_counter();
int peer_inits();
int peer_calls_main(int times);
char peer_object();

int main_init_calls = 0;

int main_init() {
  ++main_init_calls;
  return 100;
}

int main_counter() {
  static int n = main_init();
  return ++n;
}

int main_inits() { return main_init_calls; }

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

char main_object() {
  static Noisy m('m');
  return m.name;
}

int main() {
  if (peer_inits() != 0 || main_init_calls != 0) return 1;
  if (peer_counter() != 201) return 2;
  if (peer_counter() != 202) return 3;
  if (peer_inits() != 1) return 4;

  if (peer_calls_main(3) != 103) return 5;
  if (main_counter() != 104) return 6;
  if (peer_calls_main(1) != 105) return 7;
  if (main_init_calls != 1) return 8;
  if (peer_counter() != 203) return 9;
  if (peer_inits() != 1) return 10;

  if (main_object() != 'm') return 11;
  if (peer_object() != 'p') return 12;
  if (main_object() != 'm') return 13;
  if (peer_object() != 'p') return 14;
  putchar('e');
  putchar('n');
  putchar('d');
  putchar('\n');
  return 0;
}
