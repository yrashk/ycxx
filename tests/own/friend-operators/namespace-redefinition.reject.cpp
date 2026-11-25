// #1178, [basic.def.odr]/1, [class.friend]/6: a namespace-scope definition of an operator that a
// class's friend definition already defines.
namespace n {
struct A {
    friend int operator~(const A&) { return 1; }
    friend A& operator+=(A& a, int) { return a; }
    friend A& operator++(A& a);
    friend int operator*(const A&) noexcept;
};
int operator~(const A&) { return 2; } //! error
A& operator+=(A& a, int) { return a; } //! error
A& operator++(A& a) { return a; }
long operator*(const A&) noexcept; //! error
}
