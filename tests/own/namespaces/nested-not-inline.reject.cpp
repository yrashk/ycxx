// #153, ι (#337): in `namespace a::inline b`, `a` is not inline ([namespace.def]/2), so its
// members are not found in the enclosing namespace.
namespace a::inline b { int f() { return 1; } }
int main() { return f(); } //! error
