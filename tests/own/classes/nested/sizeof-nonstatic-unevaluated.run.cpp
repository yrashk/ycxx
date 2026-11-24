// #306 [class.nest] example, [expr.prim.id]: a non-static enclosing member names only in an unevaluated operand.
struct E {
    int i;
    char buf[13];
    struct N {
        int a = sizeof(i);
        int b = sizeof(buf);
        static const int c = sizeof(i);
        decltype(i) d = 5;
        int f() { return sizeof(buf); }
    };
};
int main() {
    E::N n;
    return (n.a == sizeof(int) && n.b == 13 && E::N::c == sizeof(int) && n.d == 5 && n.f() == 13) ? 0 : 1;
}
