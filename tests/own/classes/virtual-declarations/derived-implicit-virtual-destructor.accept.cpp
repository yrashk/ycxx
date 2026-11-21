struct Base {
  virtual ~Base() noexcept {}
};

struct Derived : Base {};

static_assert(__has_virtual_destructor(Base));
static_assert(__has_virtual_destructor(Derived));
