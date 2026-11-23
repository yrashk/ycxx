// [expr.dynamic.cast]: adding const does not permit dropping volatile.
struct B { virtual int f(); };
struct D : public B { int f() override; };

const D* cast(volatile B* base) { return dynamic_cast<const D*>(base); }  //! error
