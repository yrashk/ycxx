// ι (#298): constexpr dereference can initialize a constexpr reference loop variable by
// returning a global reference without reading the iterator ([stmt.ranged], [dcl.constexpr]).
// EXIT: 0
int global = 5;

struct Iterator {
  int position;
  constexpr int& operator*() const { return global; }
  Iterator& operator++() {
    ++position;
    return *this;
  }
  bool operator!=(const Iterator& other) const {
    return position != other.position;
  }
};

struct Range {
  Iterator begin() { return Iterator{0}; }
  Iterator end() { return Iterator{1}; }
};

int main() {
  Range range;
  for (constexpr int& x : range) x += 2;
  return global == 7 ? 0 : 1;
}
