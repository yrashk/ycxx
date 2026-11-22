// [expr.dynamic.cast]: the target cannot cast away constness.
struct B { virtual int f(); };
struct D : public B { int f() override; };

D* cast(const B* base) { return dynamic_cast<D*>(base); }  //! error
