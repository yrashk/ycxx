// #306 [class.mem.general]: an enclosing class with a nested-class member has the layout of the equivalent namespace-scope struct.
struct Plain { int a; char b; };
struct Outer1 { struct In { int a; char b; }; In m; };
struct Plain2 { Plain m; char c; };
struct Outer2 { struct In { int a; char b; }; In m; char c; };
struct Outer3 { struct In { int a; char b; }; };
static_assert(sizeof(Outer1) == sizeof(Plain));
static_assert(sizeof(Outer2) == sizeof(Plain2));
static_assert(sizeof(Outer3::In) == sizeof(Plain));
static_assert(alignof(Outer3::In) == alignof(Plain));
static_assert(sizeof(Outer2) >= sizeof(int) + 2);
int main() { return 0; }
