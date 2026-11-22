struct Member {
  ~Member() noexcept {}
};

struct Base {
  virtual ~Base() noexcept {}
};

struct Derived : Base {
  Member member;
};
