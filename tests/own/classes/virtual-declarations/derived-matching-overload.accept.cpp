struct Base {
  virtual int read(short) const & noexcept { return 1; }
  virtual int read(int) const & noexcept { return 2; }
};

struct Derived : Base {
  int read(int) const & noexcept override { return 3; }
};
