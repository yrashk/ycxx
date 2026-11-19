// θ step 3 (#281), spec-only: a mutable generic lambda's function call operator template is not const,
// and every specialization modifies the same copy members of the closure object
// ([expr.prim.lambda.closure], [expr.prim.lambda.capture]).
int main() {
  int start = 10;
  auto acc = [start](auto x) mutable { start += x; return start; };
  if (acc(1) != 11) return 1;
  if (acc('\2') != 13) return 2;
  if (acc(7L) != 20) return 3;
  if (start != 10) return 4;
  auto calls = [n = 0](auto) mutable { return ++n; };
  calls(1);
  calls('c');
  calls(&start);
  if (calls(0L) != 4) return 5;
  auto copy = calls;
  if (copy(1) != 5 || calls(1) != 5) return 6;
  return 0;
}
