// ι (#298): range-based for over an array with each kind of loop variable: by value (a copy, so
// changing it leaves the element alone), `int&` writing through to the elements, `const auto&`,
// `auto` and `auto&&` ([stmt.ranged]/1, the array case of the begin-expr in [stmt.ranged]/1.1,
// [dcl.type.auto.deduct]).
// EXIT: 0
int main() {
  int a[4] = {1, 2, 3, 4};

  int sum = 0;
  for (int x : a) {
    sum += x;
    x = 100;
  }
  if (sum != 10) return 1;
  if (a[0] != 1 || a[3] != 4) return 2;

  for (int& x : a) x *= 2;
  if (a[0] != 2 || a[1] != 4 || a[2] != 6 || a[3] != 8) return 3;

  sum = 0;
  for (const auto& x : a) sum += x;
  if (sum != 20) return 4;

  sum = 0;
  for (auto x : a) sum += x;
  if (sum != 20) return 5;

  for (auto&& x : a) x += 1;
  if (a[0] != 3 || a[3] != 9) return 6;

  // The reference binds to the element itself: its address is the element's.
  int i = 0;
  for (const int& x : a) {
    if (&x != &a[i]) return 7;
    ++i;
  }
  if (i != 4) return 8;

  // A one-element array, and a const array.
  int one[1] = {42};
  int n = 0;
  for (int x : one) n += x;
  if (n != 42) return 9;
  const int c[3] = {5, 6, 7};
  n = 0;
  for (auto& x : c) n += x;
  if (n != 18) return 10;
  return 0;
}
