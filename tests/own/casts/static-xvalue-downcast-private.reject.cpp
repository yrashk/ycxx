// The inverse base conversion must be accessible at the cast ([class.access.base]).
struct Base {};
class Middle : Base {};
struct Derived : Middle {};
Derived&& cast(Base&& base) {
    return static_cast<Derived&&>(static_cast<Base&&>(base));  //! error
}
