// An inverse base conversion is available to explicit casts.
struct Base {};
struct Derived : Base {};
Derived* cast(Base* base) { return base; }  //! error
