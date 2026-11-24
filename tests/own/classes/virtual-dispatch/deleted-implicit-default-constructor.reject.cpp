// XFAIL: gcc reports why each implicit constructor is deleted as further errors at its class
// #309 [class.default.ctor]/2: the implicit default constructor of a polymorphic class is deleted
// when a member has no default constructor or is a reference without an initializer, and so is
// that of a class containing such an object.
struct NoDefault {
  NoDefault(int);
};

struct Holder {
  virtual int f() { return 0; }
  NoDefault member;
};

struct Referring {
  virtual int f() { return 1; }
  int& r;
};

struct Outer {
  Holder inner;
};

void use() {
  Holder h; //! error
  Referring r; //! error
  Outer o; //! error
}
