// δ s2 (#86): const member functions called on const and non-const objects and through
// pointers to const; a non-const member function that calls a const one
// ([class.mfct.non.static], [class.this], [dcl.type.cv]).
// EXIT: 0
struct Range {
  int lo;
  int hi;

  int size() const { return hi - lo; }
  bool contains(int v) const { return v >= lo && v < hi; }
  bool overlaps(const Range* o) const { return contains(o->lo) || o->contains(lo); }
  void grow(int n) {
    if (size() + n >= 0) hi += n;
  }
};

int total(const Range* rs, int n) {
  int t = 0;
  for (int i = 0; i < n; i++) t += rs[i].size();
  return t;
}

int main() {
  const Range r = {2, 7};
  if (r.size() != 5) return 1;
  if (!r.contains(6) || r.contains(7)) return 2;
  Range m = {6, 9};
  if (!r.overlaps(&m) || !m.overlaps(&r)) return 3;
  m.grow(-4);
  if (m.size() != 3) return 4;
  m.grow(2);
  if (m.hi != 11) return 5;
  Range rs[3] = {{0, 1}, {0, 2}, {5, 10}};
  if (total(rs, 3) != 8) return 6;
  const Range* cp = &m;
  if (cp->size() != 5) return 7;
  return 0;
}
