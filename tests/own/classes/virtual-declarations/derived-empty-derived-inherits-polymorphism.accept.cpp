struct Base {
  virtual int read() const noexcept { return 1; }
};

struct EmptyDerived : Base {};

static_assert(__is_polymorphic(Base));
static_assert(__is_polymorphic(EmptyDerived));
