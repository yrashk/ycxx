// ι (#298): range-based for over a class whose begin() and end() return a user-defined iterator
// class with operator*, prefix operator++ and operator!=; the loop uses exactly those
// ([stmt.ranged]/1: `__begin != __end`, `++__begin`, `*__begin`). The iterator counts its
// increments and comparisons; an empty range runs the body zero times.
// EXIT: 0
int increments = 0;
int compares = 0;

struct Iter {
  int* p;
  int& operator*() const { return *p; }
  Iter& operator++() {
    ++increments;
    ++p;
    return *this;
  }
  bool operator!=(const Iter& o) const {
    ++compares;
    return p != o.p;
  }
};

struct List {
  int data[5];
  int size;
  List() : size(5) {
    for (int i = 0; i < 5; ++i) data[i] = i + 1;
  }
  Iter begin() { return Iter{data}; }
  Iter end() { return Iter{data + size}; }
};

// An iterator that yields values rather than references.
struct CountIter {
  int v;
  int operator*() const { return v * v; }
  CountIter& operator++() {
    ++v;
    return *this;
  }
  bool operator!=(CountIter o) const { return v != o.v; }
};

struct Squares {
  int lo;
  int hi;
  CountIter begin() const { return CountIter{lo}; }
  CountIter end() const { return CountIter{hi}; }
};

int main() {
  List l;
  int sum = 0;
  for (int x : l) sum += x;
  if (sum != 15) return 1;
  if (increments != 5 || compares != 6) return 2;

  for (int& x : l) x = -x;
  if (l.data[0] != -1 || l.data[4] != -5) return 3;

  l.size = 0;
  increments = 0;
  compares = 0;
  for (int x : l) return 4 + x;
  if (increments != 0 || compares != 1) return 5;

  Squares s{1, 4};
  sum = 0;
  for (int x : s) sum += x;
  if (sum != 1 + 4 + 9) return 6;
  return 0;
}
