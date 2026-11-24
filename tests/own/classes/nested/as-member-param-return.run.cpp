// #306 [class.nest], [class.mem]: nested class as member type, parameter type and return type of the enclosing class.
class O {
public:
    struct P { int x, y; };
    P p;
    O() : p{1, 2} {}
    P get() const { return p; }
    void put(P q) { p = q; }
    P add(const P& a, P b) { return P{a.x + b.x, a.y + b.y}; }
    static P zero() { return P{0, 0}; }
};
int main() {
    O o;
    o.put(o.add(o.get(), O::P{10, 20}));
    O::P z = O::zero();
    return (o.p.x == 11 && o.p.y == 22 && z.x == 0 && z.y == 0) ? 0 : 1;
}
