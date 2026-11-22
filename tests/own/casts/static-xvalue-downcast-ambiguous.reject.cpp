// Two base subobjects make the inverse conversion ambiguous ([conv.ptr]).
struct Base {};
struct Left : Base {};
struct Right : Base {};
struct Derived : Left, Right {};
Derived&& cast(Base&& base) {
    return static_cast<Derived&&>(static_cast<Base&&>(base));  //! error
}
