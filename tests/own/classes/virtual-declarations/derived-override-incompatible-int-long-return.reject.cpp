struct Base {
  virtual int read() const { return 1; }
};

struct Derived : Base {
  long read() const override { return 2; } //! error
};
