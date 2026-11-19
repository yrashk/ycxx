// ζ p4 (#88), from the step's review: partial ordering of templates with function parameter packs:
// a pack's pattern against every remaining argument, and a template without a trailing pack more
// specialized ([temp.deduct.partial]).
// EXIT: 0
template<class... Ts> int a(Ts...) { return 1; }
template<class... Ts> int a(Ts*...) { return 2; }
template<class T, class... Ts> int b(T, Ts...) { return 1; }
template<class T, class... Ts> int b(T*, Ts...) { return 2; }
template<class... Ts> int c(Ts...) { return 1; }
template<class T, class... Ts> int c(T, Ts...) { return 2; }
template<class T, class U> int d(T, U) { return 3; }
template<class... Ts> int d(Ts...) { return 1; }
template<class... Ts> int e(int, Ts...) { return 1; }
template<class... Ts> int e(Ts...) { return 2; }
int main() {
  int x = 0;
  if (a(&x, &x) != 2) return 1;
  if (a(&x, 1) != 1) return 2;
  if (b(&x, 1) != 2) return 3;
  if (c(1, 2) != 2) return 4;
  if (c() != 1) return 5;
  if (d(1, 2) != 3) return 6;
  if (e(1, 2) != 1) return 7;
  return 0;
}
