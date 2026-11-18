// δ s1 (#86), from the step's review: an array of unknown bound as a member
// ([class.mem.general]), arithmetic on a pointer to an incomplete class ([expr.add]), assignment
// of a class with a const member ([class.copy.assign]/7), designated and plain clauses mixed
// ([dcl.init.general]), and an empty list giving no bound ([dcl.init.aggr]).
struct S { int n; int a[]; }; //! error
struct C { const int x; };
struct I;
int f(I* p) { return p + 1 == p; } //! error
struct D { int x, y; };
int main() {
  C a{1}, b{2};
  a = b; //! error
  a = {3}; //! error
  D d{1, .y = 2}; //! error
  int e[] = {}; //! error
  return 0;
}
