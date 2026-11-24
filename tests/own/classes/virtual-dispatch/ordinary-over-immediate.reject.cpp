// #1097 [class.virtual]: an ordinary virtual cannot override a consteval virtual.
struct Base {
  virtual consteval int value() const { return 1; }
};

struct Derived : Base {
  int value() const override { return 2; } //! error
};
