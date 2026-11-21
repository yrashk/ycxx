struct Member {
  ~Member() noexcept(false) {}
};

struct Base {
  virtual ~Base() noexcept {}
};

struct Derived : Base { //! error
  Member member;
};
