// θ step 1 (#279), spec-only: once a return statement has been seen, the deduced return type can be used in
// the rest of the function, including in a recursive call ([dcl.spec.auto.general] example of
// `sum`, [dcl.type.auto.deduct]).
auto sum(int i) {
  if (i == 1)
    return i;
  else
    return sum(i - 1) + i;
}

auto fact(long n) {
  if (n <= 1) return 1L;
  return n * fact(n - 1);
}

int main() {
  if (sum(4) != 10) return 1;
  if (fact(5) != 120) return 2;
  return 0;
}
