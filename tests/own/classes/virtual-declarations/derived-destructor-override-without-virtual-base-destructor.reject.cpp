struct Base {
  virtual int read();
};

struct Derived : Base {
  ~Derived() override = default; //! error
};
