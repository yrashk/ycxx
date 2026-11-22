// [expr.static.cast], [over.match.conv], [class.conv.fct]
struct Number {
  int value;
  operator int() const { return value; }
  operator long() const { return value + 10; }
};

int main() {
  const Number number{7};
  return static_cast<int>(number) != 7 || static_cast<long>(number) != 17;
}
