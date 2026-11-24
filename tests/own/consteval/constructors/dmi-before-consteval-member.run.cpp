// #102, [class.base.init], [expr.const]: a default member initializer analysed before a member
// that a consteval default constructor initializes.
// XFAIL: gcc GCC 16 treats the member's immediate invocation as modifying `*this` from outside it
struct K {
  int v;
  consteval K() : v(1) {}
};
struct A {
  int x = 1;
  K k;
  A() {}
};
int main() {
  A a;
  return a.k.v + a.x == 2 ? 0 : 1;
}
