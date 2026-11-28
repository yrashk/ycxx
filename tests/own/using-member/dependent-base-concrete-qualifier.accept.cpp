// Member using-declarations naming a concrete class are deferred to instantiation when
// the derived class has a dependent base ([namespace.udecl]/3, [temp.dep], [temp.inst], #1553).
struct B {
  static constexpr int sf() { return 11; }
  constexpr int f() const { return 1; }
  constexpr char ov(int) const { return 0; }
  constexpr int operator()(int x) const { return x + 10; }
  template<class U> constexpr int tf(U x) const { return static_cast<int>(x) + 1; }
protected:
  constexpr int prot() const { return 7; }
};
using AliasB = B;

struct Mid : B {};

template<class T> struct Wrap : T {};

template<class T> struct D : T {
  using B::f;
  using B::sf;
  using B::ov;
  using B::operator();
  using B::tf;
  constexpr short ov(double) const { return 0; }
  static constexpr int static_v = sf();
  constexpr int call_unqual() const { return f(); }
  constexpr int call_tmpl() const { return this->template tf<int>(4); }
  struct Inner {
    static constexpr int call_outer_static() { return sf(); }
  };
};

template<class T> struct Indirect : Wrap<T> {
  using AliasB::f;
};

template<class T> struct Partial;
template<class T> struct Partial<T*> : T {
  using B::f;
};

template<class T> struct Outer {
  struct Inner : T {
    using B::f;
  };
};

template<class T> struct PrivInherit : private T {
public:
  using B::f;
};

// Unused template with concrete qualifier and dependent base is valid.
template<class T> struct Unused : T {
  using B::f;
};

struct Other {};

static_assert(D<B>::static_v == 11);
static_assert(D<B>::Inner::call_outer_static() == 11);

void check(D<B> db, D<Mid> dm, Indirect<B> ib, Partial<B*> pb, Outer<B>::Inner oi, PrivInherit<B> pi) {
  static_assert(sizeof(db.ov(1)) == sizeof(char));
  static_assert(sizeof(db.ov(1.0)) == sizeof(short));
  (void)db.f();
  (void)db.call_unqual();
  (void)db.call_tmpl();
  (void)db(5);
  (void)dm.f();
  (void)ib.f();
  (void)pb.f();
  (void)oi.f();
  (void)pi.f();
}
