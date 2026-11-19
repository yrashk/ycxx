// #194: sizeof and alignof of classes ([expr.sizeof], [expr.alignof]), laid out as Itanium C++
// ABI §2.4 has it: members in declaration order with padding, the size a multiple of the
// alignment, an empty class of size 1, a union as large as its largest member, a reference
// member as a pointer, nested classes and arrays of classes, and a class template's
// specialization.
struct E {};
struct C1 { char c; };
struct P { char c; int i; char d; };
struct L { char c; long l; };
struct N { P p; char c; };
struct A { P ps[3]; short s; };
union U { char c; long l; int i[3]; };
struct R { char c; int& r; };
struct S { short a; char b; };
class K { int x; char y; public: K(int v) : x(v), y(0) {} };
enum class Small : char { a };
struct WithEnum { Small s; int i; Small t; };
template <class T> struct Box { char tag; T value; };
static_assert(sizeof(E) == 1 && alignof(E) == 1);
static_assert(sizeof(C1) == 1);
static_assert(sizeof(P) == 12 && alignof(P) == 4);
static_assert(sizeof(L) == 16 && alignof(L) == 8);
static_assert(sizeof(N) == 16);
static_assert(sizeof(A) == 40);
static_assert(sizeof(U) == 16 && alignof(U) == 8);
static_assert(sizeof(R) == 16);
static_assert(sizeof(S) == 4 && alignof(S) == 2);
static_assert(sizeof(K) == 8);
static_assert(sizeof(WithEnum) == 12);
static_assert(sizeof(Box<long>) == 16 && sizeof(Box<char>) == 2);
static_assert(sizeof(P[2]) == 24);
int f(P& p) { return sizeof p + sizeof(N) + alignof(U); }
int main() { return 0; }
