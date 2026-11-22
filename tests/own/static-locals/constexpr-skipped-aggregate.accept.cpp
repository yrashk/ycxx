// [expr.const], [stmt.dcl]: constant evaluation may bypass a static declaration. A
// constant-initialized block-scope const int is usable on a path that reaches it.
struct Record { int value; };

constexpr int read(bool take) {
  if (!take) return 0;
  static const Record object{7};
  return object.value;
}

constexpr int integral() {
  static const int number = 9;
  static constexpr int increment = 2;
  const int* address = &number;
  return *address + increment;
}

static_assert(read(false) == 0);
static_assert(integral() == 11);

int main() { return read(true) == 7 && read(false) == 0 ? 0 : 1; }
