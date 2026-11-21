// [expr.throw], [expr.cond] Instantiate the operand and conditional result for different types.
template<class T>
T select(bool choose, T value) {
  return choose ? value : throw value;
}

int select_integer(bool choose, int value) {
  return select(choose, value);
}

int* select_pointer(bool choose, int* value) {
  return select(choose, value);
}
