struct Base {
  virtual int read() const & noexcept;
};

struct Derived : Base {
  int read() & noexcept override; //! error
};
