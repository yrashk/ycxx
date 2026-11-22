// [expr.cond] A single throw operand preserves the other operand's type and category.
void nothing();

void select(bool choose, const int& current, int* pointer) {
  static_assert(__is_same(decltype(choose ? current : throw 1), const int&));
  static_assert(__is_same(decltype(choose ? (throw 1) : static_cast<int&&>(*pointer)), int&&));
  static_assert(__is_same(decltype(choose ? pointer + 1 : throw), int*));
  static_assert(__is_same(decltype(choose ? throw 1 : throw 2), void));
  static_assert(__is_same(decltype(choose ? nothing() : throw 1), void));
}
