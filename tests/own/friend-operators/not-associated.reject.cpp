// #1178, [basic.lookup.argdep], [over.match.oper]: a hidden friend of a class that is not
// associated with the operands is not a candidate.
namespace n {
struct Target {};
enum Open { o };
enum class Closed { c };
struct Other {
    friend int operator~(const Target&) { return 1; }
    friend int operator++(Target&) { return 2; }
    friend int operator+=(Target&, int) { return 3; }
    friend int operator~(Closed) { return 4; }
    friend int operator|=(Closed&, Closed) { return 5; }
    friend int operator!(Closed) { return 6; }
    friend int operator++(Open&) { return 7; }
};
struct Enclosing {
    struct Inner { enum class Deep { d }; };
    friend int operator~(Inner::Deep) { return 8; }
};
struct Derived : Target {};
struct DerivedFriend : Target {
    friend int operator*(const Target&) { return 9; }
};
}

void f(n::Target& t, n::Closed& c, n::Open& o, n::Derived& d) {
    ~t; //! error
    ++t; //! error
    t += 1; //! error
    ~c; //! error
    c |= c; //! error
    !c; //! error
    ++o; //! error
    ~n::Enclosing::Inner::Deep::d; //! error
    ~d; //! error
    *t; //! error
}
