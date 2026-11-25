// #1178, [class.friend]/6, [basic.scope.namespace]: one operator that two classes befriend, declared
// in one and defined in the other.
// EXIT: 0
namespace n {
class B;
class A {
    int v = 5;
    friend int operator*=(A&, const B&);
    friend int operator!(const A&);

public:
    friend int operator^=(const A&, const A&) { return 9; }
};
class B {
    int w = 7;
    friend int operator*=(A& a, const B& b) { return a.v * b.w; }
    friend int operator!(const A& a) { return a.v + 1; }
};
}
int main() {
    n::A a;
    n::B b;
    if ((a *= b) != 35) return 1;
    if (!a != 6) return 2;
    if ((a ^= a) != 9) return 3;
    return 0;
}
