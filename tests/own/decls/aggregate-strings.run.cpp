// #133, from the step's review: a string literal initializes a character array element of an
// aggregate, not elided into ([dcl.init.string], [dcl.init.aggr]/16), also in arrays of arrays
// whose bound it gives, with designators, and a member through its default member initializer.
// EXIT: 0
struct S { char n[4]; int k; };
S s = {"ab", 3};
char m[2][3] = {"ab", "cd"};
char u[][4] = {"ab", "cd", "e"};
struct WithCtor { int k = 3; char n[4] = "ab"; WithCtor() {} };
int main() {
  WithCtor w;
  S l = {"xy", 1};
  S d = {.n = "q", .k = 2};
  char b[][3] = {"a", "bc"};
  return (s.n[1] - 'b') + s.k + (m[1][1] - 'd') + (u[2][0] - 'e') + (l.n[1] - 'y') + (d.n[0] - 'q') + d.k +
         (b[1][1] - 'c') + (w.n[1] - 98) + w.k - 8;
}
