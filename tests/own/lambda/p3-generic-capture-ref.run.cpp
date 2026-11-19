// θ step 3 (#281), spec-only: a generic lambda capturing by reference refers to the original variable
// from every specialization of its function call operator ([expr.prim.lambda.capture],
// [expr.prim.lambda.closure]).
int main() {
  long total = 0;
  auto acc = [&total](auto x) { total += x; return total; };
  acc(1);
  acc('\2');
  acc(30L);
  if (total != 33) return 1;
  int count = 0;
  auto tick = [&](auto) { ++count; total = 0; };
  tick(1);
  tick('a');
  tick(&count);
  if (count != 3 || total != 0) return 2;
  if (acc(5) != 5) return 3;
  return 0;
}
