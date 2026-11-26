// [stmt.return]/2, [expr.cond]/6,7.1, [class.copy.ctor]/1
int copies;

struct Counted {
  int payload;

  explicit Counted(int value) noexcept : payload(value) {}
  Counted(const Counted& other) noexcept : payload(other.payload) { ++copies; }
  Counted(Counted&&) = delete;
};

Counted choose_counted(bool copy_existing, Counted& existing, int replacement) noexcept {
  return copy_existing ? existing : Counted{replacement};
}

int main() {
  Counted existing{13};
  Counted copied = choose_counted(true, existing, 29);
  int after_copy = copies;
  Counted made = choose_counted(false, existing, 29);
  if (copied.payload != 13 || after_copy != 1) return 1;
  if (made.payload != 29 || copies != 1) return 2;
  return 0;
}
