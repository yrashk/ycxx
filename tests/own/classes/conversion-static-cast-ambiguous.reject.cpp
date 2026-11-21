// [expr.static.cast], [over.match.conv], [over.match.best]
struct Number {
  operator int() const;
  operator int&() const;
};

int read(const Number& number) {
  return static_cast<int>(number); //! error
}
