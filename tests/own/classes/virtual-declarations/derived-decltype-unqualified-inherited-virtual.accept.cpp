struct Base {
  virtual int read() const noexcept { return 1; }
};

struct Derived : Base {
  long other() const noexcept { return 2; }
};

const Derived& object();

static_assert(__is_same(decltype(object().read()), int));
