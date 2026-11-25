// #1178, [class.friend]/2, [class.friend]/7: a friend defined in a class is in the class's lexical
// scope and has access to its private members, also inherited and nested ones.
// EXIT: 0
namespace n {
class Base {
protected:
    static constexpr int mask = 15;
    int prot = 4;
};
class S : Base {
    enum class Hidden { h = 6 };
    using Int = long;
    static int calls;
    struct Priv { int p = 8; } priv;
    int v = 1;
    static int helper(int i) { return i * 2; }

public:
    friend Int operator~(const S& s) { Int i = mask; return i + s.prot; }
    friend int operator!(const S&) { ++calls; return int(Hidden::h); }
    friend S& operator++(S& s) { s.v = helper(s.v); ++calls; return s; }
    friend S& operator<<=(S& s, int n) { Priv q{n}; s.priv = q; return s; }
    friend int operator*(const S& s) { return s.priv.p + s.v; }
    friend int operator+(const S& s);
    static int count() { return calls; }
};
int S::calls = 0;
int operator+(const S& s) { return s.v + S::calls + S::mask + S::helper(1); }
}

int main() {
    n::S s;
    static_assert(__is_same(decltype(~s), long));
    if (~s != 19) return 1;
    if (!s != 6) return 2;
    if (*++s != 10) return 3;
    s <<= 30;
    if (*s != 32) return 4;
    if (n::S::count() != 2) return 5;
    if (+s != 21) return 6;
    return 0;
}
