// #1178, [over.unary]/3, [over.inc]: the unary and binary forms of one operator, and the prefix
// and postfix forms of ++ and --, are different overloads, each chosen by the operand count.
// EXIT: 0
namespace n {
struct S {
    int v = 0;
    friend int operator+(const S&) { return 1; }
    friend int operator+(const S&, const S&) { return 2; }
    friend int operator*(const S&) { return 3; }
    friend int operator*(const S&, const S&) { return 4; }
    friend int operator&(const S&) { return 5; }
    friend int operator&(const S&, const S&) { return 6; }
    friend int operator++(S& s) { return 7; }
    friend int operator++(S& s, int) { return 8; }
    friend int operator--(S&, int) { return 9; }
    friend int operator--(S&) { return 10; }
    friend int operator+=(S&, const S&) { return 11; }
};
struct BinaryOnly {
    friend int operator&(const BinaryOnly&, const BinaryOnly&) { return 1; }
};
}

int main() {
    n::S a, b;
    if (+a != 1 || a + b != 2) return 1;
    if (*a != 3 || a * b != 4) return 2;
    if (&a != 5 || (a & b) != 6) return 3;
    if (++a != 7 || --a != 10) return 4;
    if (operator++(a) != 7 || operator++(a, 0) != 8) return 5;
    if (operator--(a) != 10 || operator--(a, 0) != 9) return 6;
    if (operator+(a) != 1 || operator+(a, b) != 2 || operator+=(a, b) != 11) return 7;
    if (operator*(a) != 3 || operator&(a, b) != 6) return 8;
    n::BinaryOnly x;
    n::BinaryOnly* p = &x;
    return p == __builtin_addressof(x) && (x & *p) == 1 ? 0 : 9;
}
