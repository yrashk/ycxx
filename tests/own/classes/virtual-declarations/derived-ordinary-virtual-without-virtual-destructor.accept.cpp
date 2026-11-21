struct Base {
  virtual int read() { return 1; }
};

struct Derived : Base {
  int read() override { return 2; }
};

static_assert(!__has_virtual_destructor(Base));
static_assert(!__has_virtual_destructor(Derived));
