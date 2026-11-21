struct Base {
  virtual int read() const { return 1; }

public:
  Base() {}
};

struct Derived : Base {
  Derived() {}
};
