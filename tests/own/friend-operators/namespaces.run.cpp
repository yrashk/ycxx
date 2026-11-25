// #1178, [basic.lookup.argdep], [over.match.oper]/3: friends in nested, inline and unnamed
// namespaces are found from anywhere, beside an unrelated operator ordinary lookup finds.
// EXIT: 0
namespace outer {
namespace inner {
struct A {
    friend int operator~(const A&) { return 1; }
    friend int operator+=(A&, int) { return 2; }
};
}
inline namespace v1 {
struct B {
    friend int operator!(const B&) { return 3; }
    friend int operator+=(B&, const inner::A&) { return 4; }
};
}
struct Nest {
    struct In {
        enum E { e };
    };
    // Nest is associated with In (a member of Nest), not with In::E.
    friend int operator+(const In&) { return 5; }
    friend int operator++(In&) { return 6; }
    friend int operator+(In::E) { return 7; }
};
int operator~(const B&) { return 8; }
}
namespace {
struct Local {
    friend int operator*(const Local&) { return 9; }
    friend int operator%=(Local&, Local) { return 10; }
};
}
namespace user {
struct Unrelated {};
int operator~(Unrelated) { return 20; }
int operator!(Unrelated) { return 21; }
void operator+=(Unrelated&, int) {}
using X = outer::inner::A;
int f() {
    X a;
    outer::B b;
    outer::Nest::In in;
    if (~a != 1) return 1;
    if ((a += 1) != 2) return 2;
    if (!b != 3) return 3;
    if ((b += a) != 4) return 4;
    if (+in != 5 || ++in != 6) return 5;
    if (+outer::Nest::In::e != 0) return 6;
    if (~b != 8) return 7;
    if (~Unrelated{} != 20 || !Unrelated{} != 21) return 8;
    Local l;
    return *l == 9 && (l %= l) == 10 ? 0 : 9;
}
}

int main() { return user::f(); }
