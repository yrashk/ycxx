// δ s1 (#86), from the step's review: an array whose bound comes from its initializer at
// namespace scope ([dcl.array]/8), declarators after a class definition, where the class is
// complete ([class.mem.general]), a list-initialized const local usable in a constant expression
// ([expr.const]), and a class hidden by a variable of the same name ([basic.scope.hiding]).
// EXIT: 26
int a[] = {7, 8};
struct S { int x; } s1, *ps;
struct V { int x; };
int V = 3;
int main() {
  const int n{3};
  int arr[n] = {1, 2, 3};
  s1.x = 4;
  ps = &s1;
  struct V v{4};
  return a[1] + s1.x + ps->x + arr[2] + v.x + V;
}
