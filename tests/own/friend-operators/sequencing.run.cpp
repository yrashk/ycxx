// #1178, [over.match.oper]/2, [expr.assign]/1: an overloaded compound assignment in operator
// notation sequences its right operand before its left one.
// EXIT: 0
// XFAIL: ycxx evaluates the left operand of an overloaded compound assignment first (#1275)
struct S {
    int v = 0;
    friend S& operator+=(S& s, int i) { s.v = s.v * 100 + i; return s; }
    friend S& operator<<=(S& s, const S&) { return s; }
};
int order = 0;
S global;
S& left() { order = order * 10 + 1; return global; }
int right() { order = order * 10 + 2; return 7; }
S right_class() { order = order * 10 + 3; return S{}; }
S* left_ptr() { order = order * 10 + 4; return &global; }

int main() {
    left() += right();
    if (order != 21 || global.v != 7) return 1;
    order = 0;
    left() <<= right_class();
    if (order != 31) return 2;
    order = 0;
    *left_ptr() += right();
    if (order != 24) return 3;
    order = 0;
    (left() += right()) <<= right_class();
    return order == 321 ? 0 : 4;
}
