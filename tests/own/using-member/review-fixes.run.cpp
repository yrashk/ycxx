// Review of #1150: a using-declared copy assignment writes only its base subobject; earlier
// using-declarations join lookup in an incomplete class; a dependent base's using-declared name
// is dependent in the template definition, in a class template and a partial specialization;
// using-declared member templates are template names; a class
// object's call finds a base's operator() on a prvalue and a static one through any path; a
// using-declaration naming two operator-function-ids ([namespace.udecl]/1, /11,
// [temp.dep.expr]/3, [over.call.object], [class.member.lookup]) (#1150).
struct A { int a = 10; };
struct B { int x = 0; };
struct D1 : A, B { using B::operator=; int y = 5; };

struct S2 { static constexpr int f(int) { return 1; } };
struct D2 : S2 {
  using S2::f;
  static constexpr int f(double) { return 2; }
  static constexpr int v = f(1);
  static_assert(f(1) == 1);
};

struct Ints { int name(int) { return 3; } };
template <class Fn> struct D3 : Fn { using Fn::name; int g() { return name(1); } };
struct S3 { int name(const char*) { return 4; } };
template <class Fn> struct E3 : Fn {
  using Fn::name;
  int name(long) { return 7; }
  int g() { return name("s"); }
};
template <class T, class U> struct W3;
template <class Fn> struct W3<Fn, int> : Fn {
  using Fn::name;
  int name(long) { return 7; }
  int g() { return name("s") + name(1L); }
};

struct B4 {
  template <class T> static int st(T) { return 5; }
  template <class T> int t(T) { return 3; }
};
struct D4 : B4 {
  using B4::st;
  using B4::t;
  int t(int) { return 13; }
  int in() { return t<char>('c'); }
};

struct B8 { int operator()(int x) const { return x + 1; } };
struct D8 : B8 {};
struct S8 { static int operator()(int x) { return x; } };
struct X8 : S8 {};
struct Y8 : S8 {};
struct D8s : X8, Y8 {};

struct B10 { int operator()(int) { return 1; } int g() { return 2; } };
struct D10 : B10 { using B10::operator(), B10::g; };

int main() {
  int bad = 0;
  D1 d1;
  B b;
  b.x = 1;
  d1 = b;
  if (d1.a != 10 || d1.x != 1 || d1.y != 5) bad |= 1;
  if (D2::v != 1) bad |= 2;
  D3<Ints> d3;
  E3<S3> e3;
  W3<S3, int> w3;
  if (d3.g() != 3 || e3.g() != 4 || w3.g() != 11) bad |= 4;
  D4 d4;
  if (D4::st<int>(1) != 5 || d4.in() != 3 || d4.t<int>(1) != 3) bad |= 8;
  D8s d8s;
  if (D8{}(1) != 2 || d8s(3) != 3) bad |= 32;
  D10 d10;
  if (d10(0) != 1 || d10.g() != 2) bad |= 64;
  return bad;
}
