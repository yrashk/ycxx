// #1178, [class.friend]/7: a namespace-scope definition of a befriended operator is not in the
// scope of the class, unlike a definition in the friend declaration.
namespace n {
struct A {
    static constexpr int k = 1;
    enum E { e };
    friend int operator~(const A&) { return k; }
    friend int operator!(const A&);
    friend int operator|=(E&, int);
};
int operator!(const A&) {
    return k; //! error
}
int operator|=(A::E&, int) {
    return e; //! error
}
}
