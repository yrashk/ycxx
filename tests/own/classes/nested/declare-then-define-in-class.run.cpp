// #306 [class.mem]/[class.nest]: a member class declared in-class and defined later in the same class.
class O {
public:
    class I;
    class J { public: int v = 3; };
    class I { public: int get() const { return 40; } J j; };
    int run() { I i; return i.get() + i.j.v; }
};
int main() { O o; O::I i; return (o.run() == 43 && i.get() == 40) ? 0 : 1; }
