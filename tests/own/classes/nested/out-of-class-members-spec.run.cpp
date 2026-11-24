// #306 [class.nest]/3, [class.mfct], [class.static.data]: out-of-class definitions of members of a nested class.
class O {
public:
    class I {
    public:
        I(int);
        int f() const;
        static int s;
        static int g(int);
        int v;
    };
};
O::I::I(int x) : v(x) {}
int O::I::f() const { return v + s; }
int O::I::s = 4;
int O::I::g(int a) { return a * s; }
int main() {
    O::I i(3);
    if (i.f() != 7) return 1;
    if (O::I::g(5) != 20) return 2;
    O::I::s = 1;
    return i.f() == 4 ? 0 : 3;
}
