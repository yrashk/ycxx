// θ step 2 (#280), spec-only: an array captured by copy is copied element by element; captured by
// reference it is the same array ([expr.prim.lambda.capture]).
int main() {
  int a[3] = {1, 2, 3};
  auto copy = [a] { return a[0] + a[1] + a[2]; };
  auto ref = [&a] { return a[0] + a[1] + a[2]; };
  a[0] = 10;
  if (copy() != 6) return 1;
  if (ref() != 15) return 2;
  auto size = [a] { return sizeof(a); };
  if (size() != 3 * sizeof(int)) return 3;
  return 0;
}
