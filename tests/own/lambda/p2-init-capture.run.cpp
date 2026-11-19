// θ step 2 (#280), spec-only: init-captures declare a variable of the closure initialized from an
// expression: by copy `[y = x + 1]`, by reference `[&r = x]`, from a class object, and shadowing the
// enclosing name ([expr.prim.lambda.capture], [basic.scope.lambda]).
struct Pair { int a; int b; };

int main() {
  int x = 4;
  auto f = [y = x + 1] { return y; };
  x = 40;
  if (f() != 5) return 1;
  auto g = [&r = x] { r += 1; };
  g();
  if (x != 41) return 2;
  Pair obj{1, 2};
  auto h = [v = obj] { return v.a + v.b; };
  obj.a = 100;
  if (h() != 3) return 3;
  auto s = [x = x * 2] { return x; };
  if (s() != 82 || x != 41) return 4;
  auto both = [x, n = 3] { return x + n; };
  if (both() != 44) return 5;
  return 0;
}
