// ζ f1 (#88): partial ordering over several parameters: u(T, T) is more specialized than
// u(T, U); v(T, int) than v(T, U); w(T*, T*) than w(T*, U*); when deduction fails for the more
// specialized template, the other is used ([temp.func.order], [temp.deduct.partial],
// [temp.deduct.call]).
// EXIT: 0
template<class T, class U>
int u(T, U) { return 1; }
template<class T>
int u(T, T) { return 2; }

template<class T, class U>
int v(T, U) { return 1; }
template<class T>
int v(T, int) { return 2; }

template<class T, class U>
int w(T*, U*) { return 1; }
template<class T>
int w(T*, T*) { return 2; }

int main() {
  int i = 0;
  int j = 0;
  char c = 0;
  if (u(1, 2) != 2) return 1;
  if (u(1, 'c') != 1) return 2;
  if (u('a', 'b') != 2) return 3;
  if (v(1L, 2) != 2) return 4;
  if (v(1L, 2L) != 1) return 5;
  if (w(&i, &j) != 2) return 6;
  if (w(&i, &c) != 1) return 7;
  return 0;
}
