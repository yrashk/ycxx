// The target reference's referred-to type must retain the source object's qualifiers.
struct Base {};
struct Derived : Base {};
Derived& cast(const Base& base) { return static_cast<Derived&>(base); }  //! error
