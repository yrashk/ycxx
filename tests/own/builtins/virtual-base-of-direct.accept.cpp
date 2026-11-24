// #218: `__builtin_is_virtual_base_of(Base, Derived)` ([meta.rel], [class.mi]): whether Base is a
// virtual base class of Derived, ignoring cv. Nonvirtual bases (direct, indirect, private,
// ambiguous), the same class, unrelated classes, unions and nonclass types give false. Virtual
// base specifiers are not supported yet (#310); their cases join this test with them.
struct A {};
struct D : A {};
struct DD : D {};
struct P : private A {};
struct M1 : A {};
struct M2 : A {};
struct MM : M1, M2 {};
struct Poly { virtual void f(); };
union U {};
struct Inc;
#define VB(b, d) __builtin_is_virtual_base_of(b, d)
static_assert(!VB(A, D) && !VB(A, DD) && !VB(D, DD) && !VB(A, P) && !VB(A, MM) && !VB(M1, MM) && !VB(Poly, Poly));
static_assert(!VB(D, A) && !VB(A, A) && !VB(const D, volatile D) && !VB(const A, DD));
static_assert(!VB(int, int) && !VB(A, int) && !VB(int, A) && !VB(U, U) && !VB(A, U) && !VB(U, A));
static_assert(!VB(int, Inc) && !VB(U, Inc) && !VB(A&, D&) && !VB(A*, D*) && !VB(A[1], D[1]));
static_assert(__is_same(decltype(VB(A, D)), bool));
