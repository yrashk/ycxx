// δ s3 (#86), from the step's review: a user-declared move constructor deletes the implicit
// copy constructor ([class.copy.ctor]/6); an rvalue reference does not bind to an lvalue
// ([dcl.init.ref]); a by-value parameter and a const reference are equally good
// ([over.ics.rank]).
struct M {
  int x;
  M(int v) : x(v) {}
  M(M&& o) : x(o.x) {}
};
void f(int&& r) { r = 5; }
int g(int) { return 1; }
int g(const int&) { return 2; }
int main() {
  M a(1);
  M b = a; //! error
  int i = 1;
  f(i); //! error
  int&& rr = i; //! error
  return g(i); //! error
}
