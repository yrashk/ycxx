// ζ f1 (#88), from the step's review: a non-type parameter's default that names an earlier
// parameter takes its argument ([temp.param], [temp.arg.nontype]).
// EXIT: 0
template<int M, int N = M> int f() { return N; }
template<long M, int N = M> int g() { return N; }
int main() {
  if (f<3>() != 3) return 1;
  if (f<3, 4>() != 4) return 2;
  if (g<5L>() != 5) return 3;
  return 0;
}
