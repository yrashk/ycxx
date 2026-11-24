// [dcl.init.general]/8: a const class object can be default-initialized when its class is
// const-default-constructible.
struct Empty {};
struct Initialized {
  int a = 1;
  Empty e;
};
struct Provided {
  int v;
  Provided(int x = 2) : v(x) {}
};
struct Derived : Initialized {
  int b = 3;
};
const Empty* e() { return new const Empty; }
const Initialized* i() { return new const Initialized; }
const Provided* p() { return new const Provided; }
const Derived* d() { return new const Derived; }
