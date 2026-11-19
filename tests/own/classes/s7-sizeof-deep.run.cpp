// #194, from the review: a class layout is computed once per class, in sema and in the back end;
// laid out again per use, these 22 levels of two members each would take 4^22 steps.
// TIMEOUT: 20
// EXIT: 7
struct A0 { char c; };
struct A1 { A0 a; A0 b; };
struct A2 { A1 a; A1 b; };
struct A3 { A2 a; A2 b; };
struct A4 { A3 a; A3 b; };
struct A5 { A4 a; A4 b; };
struct A6 { A5 a; A5 b; };
struct A7 { A6 a; A6 b; };
struct A8 { A7 a; A7 b; };
struct A9 { A8 a; A8 b; };
struct A10 { A9 a; A9 b; };
struct A11 { A10 a; A10 b; };
struct A12 { A11 a; A11 b; };
struct A13 { A12 a; A12 b; };
struct A14 { A13 a; A13 b; };
struct A15 { A14 a; A14 b; };
struct A16 { A15 a; A15 b; };
struct A17 { A16 a; A16 b; };
struct A18 { A17 a; A17 b; };
struct A19 { A18 a; A18 b; };
struct A20 { A19 a; A19 b; };
struct A21 { A20 a; A20 b; };
struct A22 { A21 a; A21 b; };
static_assert(sizeof(A22) == 4194304 && alignof(A22) == 1);
int main() {
  A12 x;
  x.b.a.a.a.a.a.a.a.a.a.a.a.c = 7;
  return x.b.a.a.a.a.a.a.a.a.a.a.a.c;
}
