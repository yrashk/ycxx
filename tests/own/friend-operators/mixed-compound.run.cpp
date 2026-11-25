// #1178, [over.match.oper], [basic.lookup.argdep]: compound assignments whose operands have
// different types. The friend is found through either operand, also for an arithmetic left one.
// EXIT: 0
namespace lib {
struct Meters { int v; };
struct Feet { int v; };

struct Sum {
    double total = 0;
    friend Sum& operator+=(Sum& s, int i) { s.total += i; return s; }
    friend Sum& operator+=(Sum& s, double d) { s.total += d + 0.5; return s; }
    friend Sum& operator+=(Sum& s, const Meters& m) { s.total += m.v * 10; return s; }
    friend Sum& operator+=(Sum& s, Feet f) { s.total += f.v * 3; return s; }
    friend void operator-=(Sum& s, char) { s.total = -1; }
    friend const char* operator<<=(Sum&, const char* text) { return text; }
};

struct Delta {
    int v;
    friend int& operator+=(int& i, const Delta& d) { i += d.v; return i; }
    friend long operator-=(long& i, Delta d) { return i -= d.v; }
    friend double& operator*=(double& x, Delta&& d) { x *= d.v; return x; }
    friend bool operator&=(bool& b, Delta d) { return b = b && d.v; }
    friend int* operator>>=(int*& p, Delta d) { return p += d.v; }
};

struct From { int v; From(int i) : v(i) {} };
struct Conv {
    int v = 0;
    friend Conv& operator|=(Conv& c, const From& f) { c.v |= f.v; return c; }
};
}

int main() {
    lib::Sum s;
    s += 2;
    if (s.total != 2) return 1;
    s += 1.0;
    if (s.total != 3.5) return 2;
    s += lib::Meters{2};
    if (s.total != 23.5) return 3;
    s += lib::Feet{1};
    if (s.total != 26.5) return 4;
    s -= 'x';
    if (s.total != -1) return 5;
    if ((s <<= "ok")[1] != 'k') return 6;
    static_assert(__is_same(decltype(s -= 'x'), void));

    int i = 1;
    if (&(i += lib::Delta{4}) != &i || i != 5) return 7;
    (i += lib::Delta{1}) += 10;
    if (i != 16) return 8;
    long l = 10;
    if ((l -= lib::Delta{3}) != 7 || l != 7) return 9;
    double d = 1.5;
    if ((d *= lib::Delta{2}) != 3.0) return 10;
    bool b = true;
    if ((b &= lib::Delta{0}) || b) return 11;
    int arr[3] = {1, 2, 3};
    int* p = arr;
    p >>= lib::Delta{2};
    if (*p != 3) return 12;

    lib::Conv c;
    c |= 5;
    c |= lib::From(8);
    return c.v == 13 ? 0 : 13;
}
