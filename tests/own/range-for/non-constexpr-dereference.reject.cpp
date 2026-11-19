// ι (#298): a non-constexpr iterator dereference cannot initialize a constexpr reference
// loop variable, including when the element type is deduced ([stmt.ranged], [dcl.constexpr]).
int global = 5;

struct Iterator {
  int position;
  int& operator*() const { return global; }
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

void explicit_reference(Range& range) {
  for (constexpr int& x : range) {} //! error
}

void deduced_reference(Range& range) {
  for (constexpr auto& x : range) {} //! error
}
