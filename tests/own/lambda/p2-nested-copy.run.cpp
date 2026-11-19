// θ step 2 (#280), spec-only: an inner lambda's by-copy capture of an entity the outer lambda captured by
// copy copies the outer closure's member, not the original variable ([expr.prim.lambda.capture]).
int main() {
  int x = 1;
  auto outer = [=]() mutable {
    x += 10;
    auto inner = [x] { return x; };
    x += 100;
    return inner();
  };
  x = 1000;
  if (outer() != 11) return 1;
  if (x != 1000) return 2;
  auto outer2 = [&x] { return [=] { return x; }; };
  auto snap = outer2();
  x = 5;
  if (snap() != 1000) return 3;
  return 0;
}
