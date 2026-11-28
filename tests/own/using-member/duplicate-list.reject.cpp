// Repeated base members within a single member using-declarator list are ill-formed
// ([namespace.udecl]/8, [basic.scope.scope]/4, #1552).
struct B {
  void f();
  void g();
  void ov(int);
  void ov(double);
  void operator()(int);
  operator int();
  static void sf();
  template<class T> void tf(T);
};
using AliasB = B;

struct D1 : B {
  using B::f, B::f;                  //! error
  void f(int);
  void recover() { f(); }
};

struct D2 : B {
  using B::ov, B::ov;                //! error
};

struct D3 : B {
  using B::operator(), B::operator(); //! error
};

struct D4 : B {
  using B::operator int, B::operator int; //! error
};

struct D5 : B {
  using B::sf, B::sf;                //! error
};

struct D6 : B {
  using B::tf, B::tf;                //! error
};

struct D7 : B {
  using B::f, AliasB::f;             //! error
};

struct D8 : B {
  using B::f, B::g, B::f;            //! error
};

struct D9 : B {
  using B::f, B::g;
  using B::g;                        //! error
};

template<class T> struct T1 : B {
  using B::f, B::f;                  //! error
};

template<class T> struct T2;
template<class T> struct T2<T*> : B {
  using B::f, B::f;                  //! error
};

template<class T> struct T3 : T {
  using T::f, T::f;                  //! error
  void recover() { f(); }
};

template<class T> struct T4 : T {
  using U = T;
  using T::f, U::f;                  //! error
};

template<class T> struct T5 : T {
  using T::f;
  using T::f;                        //! error
};

template<class T> struct T6 : T {
  using T::operator(), T::operator(); //! error
};

template<class T> struct T7 : T {
  using T::operator int, T::operator int; //! error
};

void local_fn() {
  struct Local : B {
    using B::f, B::f;                //! error
  };
  (void)sizeof(Local);
}
