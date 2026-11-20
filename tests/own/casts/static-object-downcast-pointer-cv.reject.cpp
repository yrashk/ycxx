// The target pointer's pointee must retain the source pointee's qualifiers.
struct Base {};
struct Derived : Base {};
const Derived* cast(volatile Base* base) { return static_cast<const Derived*>(base); }  //! error
