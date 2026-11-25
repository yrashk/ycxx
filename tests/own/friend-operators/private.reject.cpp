// #1178, [class.access], [class.friend]: an operator that is not a friend, or is a friend of
// another class, cannot name private members.
namespace n {
class B;
class A {
    int v = 1;
    friend int operator~(const A& a) { return a.v; }
    friend int operator+=(A&, const B&);
};
class B {
    int w = 2;
    friend int operator!(const A& a) { return a.v; } //! error
    friend int operator*(const B& b) { return b.w; }
};
int operator+=(A& a, const B& b) {
    return a.v +
           b.w; //! error
}
int operator++(A& a) { return a.v; } //! error
}
