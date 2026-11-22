// The target must preserve the source object's cv-qualifiers ([expr.static.cast]).
struct Base {};
struct Derived : Base {};
const Derived&& cast(volatile Base&& base) {
    return static_cast<const Derived&&>(static_cast<volatile Base&&>(base));  //! error
}
