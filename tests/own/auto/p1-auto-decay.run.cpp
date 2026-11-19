// θ step 1 (#279), spec-only: plain `auto` decays an array to a pointer and a function to a function
// pointer; `auto&` keeps the array and function types ([dcl.type.auto.deduct],
// [temp.deduct.call], [conv.array], [conv.func]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int f(int v) { return v + 1; }

int main() {
  int arr[3] = {1, 2, 3};
  auto p = arr;
  static_assert(same<decltype(p), int*>::value);
  auto& ra = arr;
  static_assert(same<decltype(ra), int (&)[3]>::value);
  static_assert(sizeof(ra) == 3 * sizeof(int));
  ra[1] = 20;
  if (p[1] != 20) return 1;
  auto fp = f;
  static_assert(same<decltype(fp), int (*)(int)>::value);
  auto& fr = f;
  static_assert(same<decltype(fr), int (&)(int)>::value);
  if (fp(1) != 2) return 2;
  if (fr(2) != 3) return 3;
  const char s[] = "hi";
  auto ps = s;
  static_assert(same<decltype(ps), const char*>::value);
  auto lit = "abc";
  static_assert(same<decltype(lit), const char*>::value);
  if (ps[0] != 'h' || lit[2] != 'c') return 4;
  return 0;
}
