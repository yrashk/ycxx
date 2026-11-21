// XFAIL: ycxx #309 a direct polymorphic base requires virtual class layout support
struct Root { virtual int f(); };
template<class Base> struct Derived : Base {}; static_assert(sizeof(Derived<Root>) > 0);
