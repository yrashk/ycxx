// [expr.new], [expr.delete], [dcl.init.general]: rules for class types that every compiler checks.
struct Incomplete;
void release(Incomplete* p) { delete p; } //! error

struct Private {
  int v;
 private:
  ~Private();
};
void release(Private* p) { delete p; } //! error

struct NoDefault {
  NoDefault(int);
};
NoDefault* make() { return new NoDefault; } //! error

struct Plain {
  int v;
};
const Plain* make_const() { return new const Plain; } //! error
struct Partly {
  int a = 1;
  int b;
};
const Partly* make_partly() { return new const Partly; } //! error

struct Abstract {
  virtual void f() = 0;
};
Abstract* make_abstract() { return new Abstract(); } //! error

struct Hidden {
 private:
  Hidden();
};
Hidden* make_hidden() { return new Hidden(); } //! error
