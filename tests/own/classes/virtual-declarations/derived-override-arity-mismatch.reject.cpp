struct Base {
  virtual int read() const noexcept;
};

struct Derived : Base {
  int read(int) const noexcept override; //! error
};
