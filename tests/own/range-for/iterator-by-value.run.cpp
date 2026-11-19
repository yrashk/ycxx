// ι (#298), from the review: an iterator whose `operator++` returns by value; the increment is a
// discarded-value expression ([stmt.ranged], [stmt.for], [expr.context]), as in a classic `for`.
// EXIT: 0
int g[3] = {1, 2, 3};
struct It {
  int* p;
  int& operator*() const { return *p; }
  It operator++() {
    ++p;
    return *this;
  }
  bool operator!=(const It& o) const { return p != o.p; }
};
struct R {
  It begin() { return It{g}; }
  It end() { return It{g + 3}; }
};
It make() { return It{g}; }
int main() {
  int s = 0;
  R r;
  for (int x : r) s += x;
  int k = 0;
  for (make(); k < 2; ++k) s += 10;  // a class prvalue discarded in a classic `for`
  return s == 26 ? 0 : 1;
}
