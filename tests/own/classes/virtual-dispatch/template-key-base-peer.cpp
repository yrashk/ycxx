// The other unit of template-key-base.run.cpp: it constructs what that unit's table describes.
template <class T> struct Base {
  virtual ~Base() {}
  virtual int unused() { return int(sizeof(T)); }
  virtual int key() { return 0; }
};
struct Derived : Base<short> {
  Derived();
  int key() override;
};
Derived::Derived() {}

int call(Base<short>& b) { return b.unused() + b.key(); }
Base<short>& made_by_peer() {
  static Derived d;
  return d;
}
