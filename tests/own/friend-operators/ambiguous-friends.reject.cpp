// #1178, [over.match.best]: hidden friends of the two operands' classes, or of two unrelated
// bases, neither better than the other.
namespace n {
struct Right;
struct Left {
    friend int operator+=(Left&, const Right&);
    friend int operator&=(Left&, long);
    friend int operator&=(Left&, char);
};
struct Right {
    friend int operator+=(const Left&, Right&);
};
struct B1 {
    friend int operator~(const B1&) { return 1; }
    friend int operator++(B1&) { return 1; }
};
struct B2 {
    friend int operator~(const B2&) { return 2; }
    friend int operator++(B2&) { return 2; }
};
struct D : B1, B2 {};
struct V {
    friend int operator!(V) { return 1; }
    friend int operator!(const V&) { return 2; }
};
}

void f(n::Left& l, n::Right& r, n::D& d, n::V& v) {
    l += r; //! error
    l &= 1; //! error
    ~d; //! error
    ++d; //! error
    !v; //! error
}
