// A using-declared `operator[]` in a subscript expression, and a further-derived class's
// operator expressions finding the base's using-declared operators ([namespace.udecl]/1,
// [over.match.oper], [over.sub], [class.member.lookup]). (#1150)
// XFAIL: ycxx an overloaded operator[] is not considered (#219); operator expressions do not find a base's member operators (#1426)
struct B {
  bool operator==(int v) const { return v == 7; }
  int operator[](int i) const { return 100 + i; }
};
struct D : B {
  using B::operator==;
  bool operator==(const char*) const { return true; }
  using B::operator[];
};
struct E : D {};
int main() {
  D d;
  E e;
  int bad = 0;
  if (d[5] != 105) bad |= 1;
  if (!(e == 7) || e[1] != 101) bad |= 2;
  return bad;
}
