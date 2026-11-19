// ι (#94), spec-only: namespace-scope objects are destroyed after main returns, in the reverse
// order of the completion of their constructors ([basic.start.term], [basic.start.main]); an object
// assigned to in main is destroyed with its new state.
// STDOUT: ABCD
// STDOUT: M
// STDOUT: dzba
// EXIT: 0
extern "C" int putchar(int);

struct Noisy {
  char name;
  Noisy(char c) : name(c) { putchar(c - 'a' + 'A'); }
  Noisy(char c, int) : name(c) { putchar(c - 'a' + 'A'); }
  ~Noisy() {
    putchar(name);
    if (name == 'a') putchar('\n');
  }
};

Noisy a('a');
Noisy b = Noisy('b');
Noisy c{'c'};
Noisy d('d', 0);

int main() {
  putchar('\n');
  putchar('M');
  putchar('\n');
  c.name = 'z';
  return 0;
}
