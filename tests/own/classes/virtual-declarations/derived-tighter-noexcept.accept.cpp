struct Base {
  virtual int read() const { return 1; }
};

struct Derived : Base {
  int read() const noexcept override { return 2; }
};
