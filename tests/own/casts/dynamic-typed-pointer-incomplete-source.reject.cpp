// [expr.dynamic.cast]: the operand must point to a complete class at the cast.
struct B;
struct D { virtual int f(); };

D* cast(B* base) { return dynamic_cast<D*>(base); }  //! error

struct B { virtual int f(); };
