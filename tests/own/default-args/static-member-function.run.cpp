// ι (#300), from the review:  a member's default argument calls an earlier static member function ([class.static.mfct]).
// EXIT: 0
struct S { static int base() { return 4; } static int f(int x = base()) { return x; } int m(int x = base()) { return x; } };
int main() { S s; return S::f() + s.m() == 8 ? 0 : 1; }
