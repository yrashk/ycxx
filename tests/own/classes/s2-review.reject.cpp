// δ s2 (#86), from the step's review: a member defined twice outside its class
// ([basic.def.odr]), `static` on a member's definition outside the class ([class.static.mfct]).
struct S {
  int v;
  void f();
  static int n;
  static int sf();
};
void S::f() { v = 1; }
void S::f() { v = 2; } //! error
int S::n = 1;
int S::n = 2; //! error
static int S::sf() { return 0; } //! error
int main() {
  return 0;
}
