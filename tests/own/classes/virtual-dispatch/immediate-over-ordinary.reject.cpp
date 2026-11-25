// #1097 [class.virtual]: a consteval virtual cannot override an ordinary virtual.
struct Base {
  virtual int value() const { return 1; }
};

struct Derived : Base {
  consteval int value() const override { return 2; } //! error
};
