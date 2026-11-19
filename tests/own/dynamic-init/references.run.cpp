// ι (#94), spec-only: a namespace-scope reference bound by a non-constant initializer refers to
// what the function chose ([basic.start.dynamic], [dcl.init.ref]).
// XFAIL: ycxx namespace-scope references are not objects of their own yet (#367)
// EXIT: 0
int left = 1;
int right = 2;
int which;

int& pick() {
  ++which;
  return which == 1 ? right : left;
}

int& ref = pick();

int main() {
  if (&ref != &right) return 1;
  ref = 20;
  if (right != 20) return 2;
  return 0;
}
