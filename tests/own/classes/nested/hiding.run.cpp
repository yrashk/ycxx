// #306 [class.nest]/1, [basic.scope.class]: a member of the nested class hides the enclosing class's member of the same name.
struct E {
    static int v;
    typedef int T;
    struct N {
        static int v;
        typedef long T;
        int f() { return v; }
        int sz() { return sizeof(T); }
        int outer() { return E::v; }
    };
};
int E::v = 1;
int E::N::v = 2;
int main() {
    E::N n;
    if (n.f() != 2 || n.outer() != 1) return 1;
    return n.sz() == sizeof(long) ? 0 : 2;
}
