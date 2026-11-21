struct Base {
  virtual int read(int) const & noexcept;
};

struct Derived : Base {
  int read(long) const & noexcept override; //! error
};
