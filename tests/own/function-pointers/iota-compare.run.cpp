// ι (#313): function pointers compare equal exactly when they point to the same function, compare
// with the null pointer, and convert to bool ([expr.eq], [conv.bool], [over.over]).
// EXIT: 0
int f(int) { return 1; }
int f(long) { return 2; }
int g(int) { return 3; }

int main() {
  int (*a)(int) = f;
  int (*b)(int) = &f;
  int (*c)(int) = g;
  int (*n)(int) = nullptr;
  if (!(a == b)) return 1;
  if (a != b) return 2;
  if (a == c) return 3;
  if (!(a != c)) return 4;
  if (a == nullptr) return 5;
  if (nullptr == a) return 6;
  if (n != nullptr) return 7;
  if (n != 0) return 8;
  if (n) return 9;
  if (!a) return 10;
  if (a == &g || c != &g) return 11;
  bool live = c;
  if (!live) return 12;
  // Null checked before calling.
  int total = 0;
  int (*fns[3])(int) = {f, nullptr, g};
  for (int i = 0; i < 3; ++i)
    if (fns[i]) total += fns[i](0);
  if (total != 4) return 13;
  n = a;
  if (n != a || n(0) != 1) return 14;
  return 0;
}
