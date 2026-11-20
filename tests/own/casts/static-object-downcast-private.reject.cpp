// An outsider cannot undo a base conversion through a private edge ([class.access.base]/4).
struct Base {};
class Middle : Base {};
struct Derived : Middle {};
Derived* cast(Base* base) { return static_cast<Derived*>(base); }  //! error
