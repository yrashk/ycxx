// θ step 2 (#280), from the review: naming a variable or a member in an unevaluated operand
// captures nothing ([expr.prim.lambda.capture]/8, [expr.context]/1).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };
struct S {
  int v;
  int f() { return [] { return int(sizeof(v)); }(); }
};
int main() {
  long x = 1;
  char c = 'a';
  auto a = [] { return sizeof(x); };
  auto b = [] { return same<decltype(x), long>::value; };
  auto d = [=] { return int(sizeof(x)) + c; };
  if (a() != sizeof(long) || !b() || S{1}.f() != sizeof(int)) return 1;
  if (sizeof(d) != 1 || d() != int(sizeof(long)) + 'a') return 2;
  return 0;
}
