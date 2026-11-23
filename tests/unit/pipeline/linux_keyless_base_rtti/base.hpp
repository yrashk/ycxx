struct B {
  virtual int f() = 0;
};

struct D : B {
  int payload;
  int f() override;
};
