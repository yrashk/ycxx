// A private second base subobject still makes the conversion ambiguous ([conv.ptr]/3).
struct Base {};
struct Left : Base {};
class Right : Base {};
struct Derived : Left, Right {};
Derived& cast(Base& base) { return static_cast<Derived&>(base); }  //! error
