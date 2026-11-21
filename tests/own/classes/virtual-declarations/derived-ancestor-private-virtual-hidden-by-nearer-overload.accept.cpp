class Base {
  virtual int read() const noexcept { return 1; }
};

struct Middle : Base {
  int read(int value) const noexcept { return value; }
};

struct Derived : Middle {
  int read() const noexcept override { return 3; }
};
