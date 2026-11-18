// #133, from the step's review: a floating local does not stop constant evaluation in its
// function: array bounds and static_assert after it are still constant ([expr.const]).
// EXIT: 0
int main() {
  double d;
  int a[3] = {1, 2, 3};
  static_assert(1 + 1 == 2);
  constexpr int n = 3;
  return a[2] - n;
}
