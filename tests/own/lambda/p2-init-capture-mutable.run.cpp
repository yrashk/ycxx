// θ step 2 (#280), spec-only: an init-capture with no counterpart outside the lambda, kept as closure
// state that a `mutable` lambda updates between calls ([expr.prim.lambda.capture],
// [expr.prim.lambda.closure]).
int main() {
  auto next = [n = 0]() mutable { return ++n; };
  next();
  next();
  if (next() != 3) return 1;
  int seed = 10;
  auto gen = [cur = seed, step = 5]() mutable { int r = cur; cur += step; return r; };
  if (gen() != 10 || gen() != 15 || gen() != 20) return 2;
  if (seed != 10) return 3;
  return 0;
}
