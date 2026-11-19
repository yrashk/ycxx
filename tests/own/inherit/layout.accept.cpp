// #308: the layout of classes with bases (Itanium C++ ABI §2.4, as Clang lays them out): bases
// first in declaration order, an empty base at offset 0 unless another subobject of its type is
// there, and a class that is not POD for layout leaving its tail padding to the derived class's
// members.
struct P { int i; char c; };             struct DP : P { char d; };
struct U { int i; char c; U() {} };      struct DU : U { char d; };
struct X { int i; char c; ~X() {} };     struct DX : X { char d; };
struct I { int i = 0; char c; };         struct DI : I { char d; };
struct E {};                             struct DE : E { int i; };
struct DE2 : E { E e; int i; };
struct E2 {};                            struct F : E, E2 { char c; };
struct G : E { E2 x; char c; };
struct H : P {};                         struct DH : H { char d; };
struct DQ : U { U u; char d; };
struct A { long a; };                    struct B { char b; };
struct MI : A, B { char m; };
struct MI2 : B, A { char m; };
struct Deep : DU { char e; };
static_assert(sizeof(DP) == 12 && sizeof(DU) == 8 && sizeof(DX) == 8 && sizeof(DI) == 8);
static_assert(sizeof(DE) == 4 && sizeof(DE2) == 8 && sizeof(F) == 1 && sizeof(G) == 2);
static_assert(sizeof(DH) == 12 && sizeof(DQ) == 20);
static_assert(sizeof(MI) == 16 && alignof(MI) == 8 && sizeof(MI2) == 24);
static_assert(sizeof(Deep) == 8);
int main() { return 0; }
