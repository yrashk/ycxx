// #308: a name found in more than one base is ambiguous ([class.member.lookup]/6), as is a
// conversion to a base that occurs more than once ([conv.ptr]/3); a base is a complete class
// named once ([class.derived.general]/2, [class.mi]/3).
struct A { int x; int f(); static int s; };
struct B { int x; };
struct L : A {};
struct R : A {};
struct D : A, B {};
struct Diamond : L, R {};
int use(D& d, Diamond& m) {
  int r = d.x;   //! error
  r += m.s;      // one static member: not ambiguous
  A* p = &m;     //! error
  return r + (p != nullptr);
}
struct I;
struct Bad1 : I {};  //! error
struct Bad2 : A, A {};  //! error
int main() { return 0; }
