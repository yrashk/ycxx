// δ s3 (#86): a reference without an initializer, a non-const lvalue reference bound to a
// temporary or to a const object ([dcl.ref], [dcl.init.ref]).
struct S {
  int v;
  S(int x) : v(x) {}
};

void take(S& s) { s.v = 0; }
void bump(int& i) { ++i; }

int main() {
  int& dangling; //! error
  int& r = 5; //! error
  S& s = S(1); //! error
  take(S(2)); //! error
  bump(3); //! error
  const int k = 1;
  int& rk = k; //! error
  return 0;
}
