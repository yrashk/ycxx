// [expr.static.cast], [over.match.conv], [class.conv.fct]
struct Number {
  int value;
  operator int() { return value; }
};

int main() {
  return static_cast<int>(Number{7}) != 7;
}
