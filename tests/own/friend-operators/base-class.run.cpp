// #1178, [basic.lookup.argdep]/3: a class's direct and indirect bases are associated with it, so
// a base's hidden friends apply to a derived operand.
// EXIT: 0
namespace a {
struct Base {
    int v = 1;
    friend int operator~(const Base& b) { return b.v + 10; }
    friend bool operator!(Base b) { return b.v == 0; }
    friend Base& operator++(Base& b) { ++b.v; return b; }
    friend Base& operator+=(Base& b, int i) { b.v += i; return b; }
    enum class Bits { x = 1, y = 2 };
    friend Bits operator|(Bits p, Bits q) { return Bits(int(p) | int(q)); }
    friend Bits& operator|=(Bits& p, Bits q) { return p = p | q; }
};
}
namespace b {
struct Mid : private a::Base {
    int through() { return ~static_cast<a::Base&>(*this) + ~*this; }
};
struct Derived : a::Base {
    friend int operator+(const Derived& d) { return d.v + 1000; }
    friend int operator&=(Derived& d, const a::Base& o) { return d.v & o.v; }
};
struct Leaf : Derived {};
}

int main() {
    b::Leaf l;
    if (~l != 11) return 1;
    if (!l) return 2;
    a::Base& r = ++l;
    if (&r != &l || l.v != 2) return 3;
    (l += 3) += 4;
    if (l.v != 9) return 4;
    if (+l != 1009) return 5;
    if ((l &= l) != 9) return 6;
    if (~b::Leaf{} != 11) return 7;
    b::Leaf::Bits m = b::Leaf::Bits::x;
    m |= b::Derived::Bits::y;
    if (int(m) != 3) return 8;
    return b::Mid{}.through() == 22 ? 0 : 9;
}
