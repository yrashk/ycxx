// #157 [dcl.fct.def.default], [class.copy.ctor], [over.match.funcs.general]/8: uses of deleted
// defaulted members, and functions that cannot be defaulted. GCC also locates the implicitly deleted
// definition, so each class is on its use's line.
struct NoCopy { NoCopy(); NoCopy(const NoCopy&) = delete; NoCopy& operator=(const NoCopy&) = delete; };
struct A { A(); A(const A&) = default; NoCopy n; }; void copy_a(const A& a) { A b(a); } //! error
struct B { B(); B(B&&) = default; NoCopy n; }; void move_b(B& a) { B b(static_cast<B&&>(a)); } //! error
struct C { C& operator=(C&&) = default; NoCopy n; }; void assign_c(C& a, C& b) { a = static_cast<C&&>(b); } //! error
struct D { D() = default; int& r; }; void default_d() { D d; } //! error
struct E { E() = default; const int c; }; void default_e() { E e; } //! error
struct F { F(F&&) = default; int x; }; void copy_f(const F& a) { F b(a); } //! error
struct G { template <class T> G(const T&) = default; }; //! error
struct H { H(int) = default; }; //! error
struct I { I(const I&, int = 0) = default; }; //! error
struct J { void operator=(const J&) = default; }; //! error
struct K { K& operator=(K) = default; }; //! error
template <class T> struct L { L(); L(const L&) = default; T t; }; void copy_l(const L<NoCopy>& a) { L<NoCopy> b(a); } //! error
class P { ~P(); }; struct M { M(); ~M() = default; P p; }; void end_m(M* m) { m->~M(); } //! error
