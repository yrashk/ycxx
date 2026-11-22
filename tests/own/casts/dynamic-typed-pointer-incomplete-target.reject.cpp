// [expr.dynamic.cast]: the target class must be complete at the cast.
struct B { virtual int f(); };
struct D;

D* cast(B* base) { return dynamic_cast<D*>(base); }  //! error

struct D : public B { int f() override; };
