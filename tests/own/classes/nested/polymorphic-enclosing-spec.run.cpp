// #306 [class.nest], [class.virtual]: nested class of a polymorphic enclosing class, defined out of class with a base.
struct SomeBase {
    virtual int code() const { return 1; }
    virtual ~SomeBase() {}
};
class B {
public:
    class failure;
    virtual ~B();
    virtual int id() const { return 10; }
};
B::~B() {}
class B::failure : public SomeBase {
public:
    int code() const override { return 99; }
};
class D : public B {
public:
    int id() const override { return 11; }
};
int main() {
    B::failure f;
    const SomeBase& s = f;
    D d;
    B& b = d;
    if (s.code() != 99) return 1;
    if (b.id() != 11) return 2;
    return 0;
}
