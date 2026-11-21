class Base {
  virtual int read() const noexcept { return 1; }
};

struct Derived : Base {
  int read() const noexcept override { return 2; }
};
