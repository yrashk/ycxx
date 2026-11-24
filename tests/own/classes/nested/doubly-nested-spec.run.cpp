// #306 [class.nest]/1: A::B::C doubly nested, lookup through both enclosing classes, out-of-class members.
struct A {
    static int a;
    struct B {
        static int b;
        struct C {
            int c;
            int sum() const { return a + b + c; }
            int again() const;
        };
        C make(int x) { return C{x}; }
    };
};
int A::a = 1;
int A::B::b = 10;
int A::B::C::again() const { return sum() * 2; }
int main() {
    A::B bb;
    A::B::C c = bb.make(100);
    return (c.sum() == 111 && c.again() == 222) ? 0 : 1;
}
