// ζ f1 (#88), from the step's review: a parameter whose type becomes an array after
// substitution is adjusted to a pointer ([dcl.fct]/5, [temp.deduct.general]).
// EXIT: 0
template<class T> int f(T t) { return t[1]; }
int main() {
  int a[3] = {1, 2, 3};
  if (f<int[3]>(a) != 2) return 1;
  return 0;
}
