// [expr.static.cast], [over.match.conv], [class.conv.fct]
struct Number {
  operator int() { return 7; }
};

int read(const Number& number) {
  return static_cast<int>(number); //! error
}
