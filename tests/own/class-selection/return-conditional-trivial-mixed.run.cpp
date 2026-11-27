// [stmt.return]/2, [expr.cond]/6,7.1, [class.temporary]/3
struct Value {
  int payload;
};

Value choose_value(bool copy_existing, Value& existing, int replacement) noexcept {
  return copy_existing ? existing : Value{replacement};
}

int main() {
  Value existing{17};
  Value copied = choose_value(true, existing, 46);
  Value made = choose_value(false, existing, 46);
  if (copied.payload != 17) return 1;
  if (made.payload != 46) return 2;
  return 0;
}
