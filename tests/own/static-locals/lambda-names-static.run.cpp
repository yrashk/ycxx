// ι (#291), from the review: a static local is no local entity, so a lambda names it rather than
// capturing it, with or without a capture-default ([expr.prim.lambda.capture]/7, [basic.pre]).
// EXIT: 0
int f() {
  static int c = 0;
  auto byval = [=] { return c; };
  auto mut = [=]() mutable { return ++c; };
  auto byref = [&] { return ++c; };
  auto none = [] { return ++c; };
  c = 10;
  int a = byval(), b = mut(), d = byref(), e = none();
  return a * 1000000 + b * 10000 + d * 100 + e;
}
int main() { return f() == 10111213 && sizeof([] {}) == 1 ? 0 : 1; }
