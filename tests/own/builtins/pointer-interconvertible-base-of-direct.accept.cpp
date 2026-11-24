// #218: `__is_pointer_interconvertible_base_of(Base, Derived)` ([meta.rel], [basic.compound]):
// Derived is unambiguously derived from Base and a Derived object is pointer-interconvertible with
// its Base subobject (Derived is standard-layout), or both name the same nonunion class, ignoring
// cv. Access does not matter.
struct A {};
struct D : A {};
struct DD : D {};
struct PD : private A {};
struct F { int x; };
struct DF : F {};             // standard-layout, the members in the base
struct G : A { int y; };      // standard-layout, the members in the derived class
struct H : F { int z; };      // members in two classes: not standard-layout
struct I : A { A a; };        // its first member's type is a base: not standard-layout
struct Poly { virtual void f(); };
struct M1 : A {};
struct M2 : A {};
struct MM : M1, M2 {};        // A twice: not standard-layout, and ambiguous
struct B1 {};
struct B2 {};
struct TwoB : B1, B2 {};
union U {};
union UI;
struct Inc;
#define PI(b, d) __is_pointer_interconvertible_base_of(b, d)
static_assert(PI(A, D) && PI(A, DD) && PI(D, DD) && PI(A, PD) && PI(F, DF) && PI(A, G) && PI(B1, TwoB) && PI(B2, TwoB));
static_assert(!PI(F, H) && !PI(A, I) && !PI(A, MM) && !PI(M1, MM) && !PI(Poly, A) && !PI(A, Poly));
static_assert(PI(A, A) && PI(const A, volatile A) && PI(H, H) && PI(Poly, Poly) && PI(Inc, Inc) && PI(const D, DD));
static_assert(!PI(D, A) && !PI(A, F) && !PI(const D, A) && !PI(int, int) && !PI(U, U) && !PI(UI, UI));
static_assert(!PI(int, Inc) && !PI(U, Inc) && !PI(A&, D&) && !PI(A*, D*));
static_assert(__is_same(decltype(PI(A, D)), bool));
