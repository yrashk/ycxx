// #1178, [basic.lookup.argdep], [class.access.base]: a private base's hidden friend is found, and
// the conversion of the operand to the base is then inaccessible.
struct Base {
    friend int operator~(const Base&) { return 1; }
    friend Base& operator++(Base& b) { return b; }
    friend Base& operator+=(Base& b, int) { return b; }
    friend int operator*=(int i, const Base&) { return i; }
};
class Derived : Base {};

void f(Derived& d) {
    ~d; //! error
    ++d; //! error
    d += 1; //! error
    1 *= d; //! error
}
