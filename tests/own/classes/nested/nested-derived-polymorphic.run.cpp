// #306 [class.nest]/3, [class.virtual]: out-of-class definition of a nested class derived from a polymorphic base.
struct Base {
    virtual int f() const { return 1; }
    virtual ~Base() {}
};
class O {
public:
    class I;
};
class O::I : public Base {
public:
    int f() const override { return 2; }
    ~I() override {}
};
int main() {
    O::I i;
    const Base& b = i;
    const Base* p = &i;
    return b.f() == 2 && p->f() == 2 ? 0 : 1;
}
