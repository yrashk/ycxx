// The other side of mixed.run.cpp, compiled by Clang.
extern "C" int putchar(int);

int main_counter();
int main_inits();

int peer_init_calls = 0;

int peer_init() {
  ++peer_init_calls;
  return 200;
}

int peer_counter() {
  static int n = peer_init();
  return ++n;
}

int peer_inits() { return peer_init_calls; }

int peer_calls_main(int times) {
  int last = 0;
  for (int i = 0; i < times; i++) last = main_counter();
  return last;
}

struct PeerNoisy {
  char name;
  PeerNoisy(char n) : name(n) {
    putchar('+');
    putchar(name);
    putchar('\n');
  }
  ~PeerNoisy() {
    putchar('-');
    putchar(name);
    putchar('\n');
  }
};

char peer_object() {
  static PeerNoisy p('p');
  return p.name;
}
