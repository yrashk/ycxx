struct Base {
  virtual int read() const { return 1; }

private:
  Base() {}
};

struct Derived : Base {
  Derived() {} //! error
};
