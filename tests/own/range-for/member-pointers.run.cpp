// ι (#298): range-based for over a class whose member begin() and end() return pointers; a
// const object uses the const overloads, a non-const one the non-const overloads, chosen by the
// object expression of `__range.begin()`; begin and end are each called once, not once per
// iteration ([stmt.ranged]/1, [over.match.funcs]).
// XFAIL: ycxx mutable data members (#373)
// EXIT: 0
struct Buf {
  int a[4];
  int n;
  int nonconst_calls;
  Buf() : n(3), nonconst_calls(0) {
    a[0] = 1;
    a[1] = 2;
    a[2] = 3;
    a[3] = 99;
  }
  int* begin() { ++nonconst_calls; return a; }
  int* end() { return a + n; }
  const int* begin() const { return a; }
  const int* end() const { return a + n; }
};

struct Counted {
  int a[2];
  mutable int calls;
  Counted() : calls(0) {
    a[0] = 7;
    a[1] = 8;
  }
  const int* begin() const { ++calls; return a; }
  const int* end() const { ++calls; return a + 2; }
};

int main() {
  Buf b;
  int sum = 0;
  for (int x : b) sum += x;
  if (sum != 6) return 1;
  if (b.nonconst_calls != 1) return 2;

  for (int& x : b) x += 10;
  if (b.a[0] != 11 || b.a[2] != 13 || b.a[3] != 99) return 3;

  const Buf& cb = b;
  sum = 0;
  for (const int& x : cb) sum += x;
  if (sum != 36) return 4;
  if (b.nonconst_calls != 2) return 5;

  const Counted c;
  sum = 0;
  for (auto x : c) sum += x;
  if (sum != 15) return 6;
  if (c.calls != 2) return 7;
  return 0;
}
