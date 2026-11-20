// A definition after the cast does not make the target complete at the expression.
struct Base {};
struct Derived;
Derived* cast(Base* base) { return static_cast<Derived*>(base); }  //! error
struct Derived : Base {};
