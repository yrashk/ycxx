struct Member {
  ~Member() noexcept(false) {}
};

struct Base {
  virtual ~Base() noexcept {}
};

struct Derived : Base {
  Member member;
  ~Derived() noexcept override {}
};
