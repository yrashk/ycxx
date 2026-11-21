// [class.static.data], [dcl.constexpr], [dcl.init.aggr] (#262)
// EXIT: 0
struct Pt { int a, b; };
struct S { static constexpr Pt q = Pt{7, 8}; };
int main() { const Pt* r = &S::q; return S::q.b == 8 && r->a == 7 ? 0 : 1; }
