// θ step 2 (#280), spec-only: a pointer captured by copy: the closure holds a copy of the pointer, through
// which the pointee can change even without `mutable` ([expr.prim.lambda.capture],
// [expr.prim.lambda.closure]).
int main() {
  int x = 1, y = 2;
  int* p = &x;
  auto write = [p](int v) { *p = v; };
  p = &y;
  write(10);
  if (x != 10 || y != 2) return 1;
  auto read = [p] { return *p; };
  y = 20;
  if (read() != 20) return 2;
  return 0;
}
