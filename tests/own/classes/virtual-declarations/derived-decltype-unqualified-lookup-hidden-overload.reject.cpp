struct Base {
  virtual int read() const noexcept { return 1; }
  long read(int value) const noexcept { return value; }
};

struct Derived : Base {
  int read() const noexcept override { return 2; }
};

const Derived& object();

using OverrideResult = decltype(object().read());
using OverloadResult = decltype(object().read(1)); //! error
