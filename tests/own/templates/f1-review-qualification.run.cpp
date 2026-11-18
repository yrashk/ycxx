// ζ f1 (#88), from the step's review: deduction through a multi-level qualification
// conversion ([temp.deduct.call]/4.2, [conv.qual]): `int**` to `const T* const*`.
// EXIT: 0
template<class T> int f(const T* const* p) { return **p; }
int main() {
  int x = 4;
  int* q = &x;
  int** p = &q;
  if (f(p) != 4) return 1;
  return 0;
}
