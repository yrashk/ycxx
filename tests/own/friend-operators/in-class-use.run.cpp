// #1178, [class.mem.general], [class.friend], [temp.inst]: hidden friend operators used inside the
// class that declares them, in complete-class contexts and, once declared, in the class body.
// EXIT: 0
struct A {
    enum E : unsigned { none, x = 1, y = 2 };
    int early(E e) { return ~e + !*this; }  // declared below: the body is a complete-class context
    E def = E(~x & 3u);
    static int arg(E e = ~y) { return e & 3; }
    friend constexpr E operator~(E e) noexcept { return E(~unsigned(e) & 3); }
    friend int operator!(const A& a) { return a.n; }
    friend A& operator+=(A& a, int i) { a.n += i; return a; }
    using R = decltype(~x);
    static_assert(noexcept(~x));
    A& add(int i) { return (*this += i) += 1; }
    int operator-() const { return !*this; }
    int n = 10;
};
static_assert(__is_same(A::R, A::E));

int main() {
    A a;
    if (a.early(A::x) != 12) return 1;
    if (a.def != A::y || A::arg() != A::x) return 2;
    if (!a.add(4) != 15 || -a != 15) return 3;
    return 0;
}
