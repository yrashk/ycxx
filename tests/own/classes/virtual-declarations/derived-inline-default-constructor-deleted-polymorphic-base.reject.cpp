struct Base {
  virtual int read() const { return 1; }

public:
  Base() = delete;
};

struct Derived : Base {
  Derived() {} //! error
};
