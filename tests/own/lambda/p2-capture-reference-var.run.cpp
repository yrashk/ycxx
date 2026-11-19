// θ step 2 (#280), spec-only: capturing a variable of reference type by copy copies the referred-to
// object; capturing it by reference refers to that object ([expr.prim.lambda.capture]).
int main() {
  int x = 1;
  int& r = x;
  auto copy = [r] { return r; };
  auto ref = [&r] { return r; };
  x = 2;
  if (copy() != 1) return 1;
  if (ref() != 2) return 2;
  auto set = [&r] { r = 3; };
  set();
  if (x != 3) return 3;
  auto mut = [r]() mutable { r = 99; return r; };
  if (mut() != 99 || x != 3) return 4;
  auto all = [=] { return r; };
  x = 4;
  if (all() != 3) return 5;
  return 0;
}
