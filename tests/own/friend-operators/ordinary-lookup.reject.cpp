// #1178, [class.friend]/7, [namespace.memdef]/3: a friend declaration does not make the operator
// visible to ordinary or qualified lookup.
namespace n {
struct A {
    enum E { e };
    friend int operator~(const A&) { return 1; }
    friend int operator!(E) { return 2; }
    friend A& operator++(A& a) { return a; }
    friend A& operator+=(A& a, int) { return a; }
    friend int operator*(const A&);
};
int f1(A& a) { return n::operator~(a); } //! error
int f2(A& a) { return (operator!)(A::e); } //! error
auto p = &operator++; //! error
}
void f3(n::A& a) { n::operator+=(a, 1); } //! error
int f4(n::A& a) { return n::operator*(a); } //! error
int f5(n::A& a) { return n::A::operator~(a); } //! error
int f6(n::A& a) { return a.operator~(); } //! error
