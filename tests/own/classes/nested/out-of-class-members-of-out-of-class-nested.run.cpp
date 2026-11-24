// #306 [class.nest]/3, [class.mfct]: members of a nested class that is itself defined outside the enclosing class.
class O { public: class I; };
class O::I {
public:
    I(int);
    int f() const;
    static int s;
    int v;
};
O::I::I(int x) : v(x) {}
int O::I::f() const { return v * s; }
int O::I::s = 4;
int main() { O::I i(2); return i.f() == 8 ? 0 : 1; }
