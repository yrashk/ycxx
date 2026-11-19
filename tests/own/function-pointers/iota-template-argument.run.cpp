// ι (#313): a template parameter of pointer-to-function type is the target of an overload set
// given as its template argument ([over.over], [temp.arg.nontype], [expr.call]).
// EXIT: 0
int f(int x) { return x + 1; }
int f(long x) { return static_cast<int>(x) + 2; }
int g(int x) { return x * 10; }

template <int (*F)(int)> int call() { return F(1); }
template <int (*F)(long)> int call_long(long v) { return F(v); }
template <int (&R)(int)> int call_ref() { return R(2); }

int main() {
  if (call<f>() != 2) return 1;
  if (call<&f>() != 2) return 2;
  if (call<g>() != 10) return 3;
  if (call_long<f>(5) != 7) return 4;
  if (call_ref<f>() != 3) return 5;
  if (call_ref<g>() != 20) return 6;
  return 0;
}
