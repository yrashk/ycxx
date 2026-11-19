// ι (#94): a ycxx object and a Clang object, each with dynamically initialized objects and
// destructors, linked into one program: both units are initialized before main and destroyed
// after it ([basic.start.dynamic], [basic.start.term]; Mach-O `__mod_init_func`, `__cxa_atexit`).
// PEER: peer-peer.cpp
// STDOUT: Mh
// EXIT: 0
extern "C" int putchar(int);
int peer_value();
int counter;
int bump() { return ++counter; }
struct Here { int v; Here() : v(bump()) {} ~Here() { putchar('h'); putchar('\n'); } };
Here here;
int main() {
  putchar('M');
  if (here.v < 1 || peer_value() != 42) return 1;
  return 0;
}
