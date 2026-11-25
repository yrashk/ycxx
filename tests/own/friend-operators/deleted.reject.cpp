// #1178, [dcl.fct.def.delete], [over.match.oper]: selecting a deleted hidden friend is ill-formed;
// a deleted friend that is not selected is harmless.
namespace n {
struct A {
    enum class E { e };
    friend int operator+(const A&) = delete;
    friend int operator~(const A&) = delete;
    friend int operator~(A&&) { return 1; }
    friend bool operator!(A) = delete;
    friend int operator*(A&) = delete;
    friend int operator&(const A&) = delete;
    friend A& operator++(A&) = delete;
    friend A& operator--(A&) = delete;
    friend int operator--(A&& a) { return 0; }
    friend A& operator+=(A&, int) = delete;
    friend A& operator+=(A& a, double) { return a; }
    friend A& operator>>=(A& a, const A&) = delete;
    friend E operator~(E) = delete;
    friend E& operator^=(E&, E) = delete;
};
}

void f(n::A& a, n::A::E& e) {
    +a; //! error
    ~a; //! error
    (void)~n::A{};
    !a; //! error
    *a; //! error
    &a; //! error
    ++a; //! error
    --a; //! error
    --n::A{};
    a += 1; //! error
    a += 1.0;
    a >>= a; //! error
    ~e; //! error
    e ^= e; //! error
}
