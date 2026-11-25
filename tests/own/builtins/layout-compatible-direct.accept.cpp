// #218: `__is_layout_compatible(T, U)` ([basic.types.general], [dcl.enum], [class.mem.general]):
// the same type ignoring cv, enumerations with the same underlying type, or standard-layout
// structs whose common initial sequence is all of both (members of layout-compatible types, the
// same alignment, the same bit-field widths), or standard-layout unions whose members pair up.
struct S1 { int a; char b; };
struct S2 { int x; char y; };
struct S3 { int a; int b; };
struct E1 {};
struct E2 {};
struct B { int x; };
struct DB : B {};
struct C1 { int x; };
struct P1 { int a; private: int b; };   // mixed access: not standard-layout
struct P2 { int a; private: int b; };
struct alignas(16) CA { int a; };      // the class's own alignment is not part of the sequence
struct I1 { int a; };
struct BF1 { int a : 3; };
struct BF2 { int a : 3; };
struct BF3 { int a : 4; };
union U1 { int a; char b; };
union U2 { char c; int d; };
union U3 { int a; };
enum EA : int {};
enum EB : int {};
enum class EC : int {};
enum ED : long {};
struct Nest1 { S1 s; };
struct Nest2 { S2 s; };
struct V1 { virtual void f(); int a; };
struct V2 { virtual void f(); int a; };
struct CI { const int a; };
struct Ar1 { int a[2]; };
struct Ar2 { int b[2]; };
struct G : E1 { int y; };    // an empty base and members in the derived class
struct K : E1 {};
struct Inc;
#define LC(a, b) __is_layout_compatible(a, b)
static_assert(LC(int, int) && LC(const int, int) && LC(int&, int&) && LC(void, const void) && LC(int(), int()));
static_assert(LC(int[3], int[3]) && LC(const int[3], int[3]) && LC(int[], int[]) && LC(int*, int*));
static_assert(!LC(int, unsigned) && !LC(int&, const int&) && !LC(int[3], int[]) && !LC(char, signed char));
static_assert(LC(S1, S2) && LC(E1, E2) && LC(DB, C1) && LC(DB, B) && LC(CA, I1) && LC(BF1, BF2) && LC(Nest1, Nest2));
static_assert(LC(CI, I1) && LC(Ar1, Ar2) && LC(V1, V1) && LC(P1, P1) && LC(const S1, volatile S2));
static_assert(!LC(S1, S3) && !LC(P1, P2) && !LC(BF1, BF3) && !LC(BF1, I1) && !LC(V1, V2));
static_assert(LC(U1, U2) && !LC(U1, U3) && !LC(U1, S1));
static_assert(LC(EA, EB) && LC(EA, EC) && !LC(EA, ED) && !LC(EA, int));
static_assert(LC(Inc[], Inc[]) && LC(Inc*, Inc*) && !LC(S1[2], S2[2]) && !LC(int S1::*, int S2::*));
static_assert(LC(G, I1) && LC(K, E2) && !LC(G, S1));
static_assert(__is_same(decltype(LC(int, int)), bool));
