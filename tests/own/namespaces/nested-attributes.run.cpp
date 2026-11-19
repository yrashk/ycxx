// #153, ι (#337): attributes in a namespace definition are not names ([namespace.def]/1,
// [dcl.attr.grammar]); a nested definition reopens an attributed namespace.
// EXIT: 0
namespace [[maybe_unused]] outer { int f() { return 1; } }
namespace outer::inner { int g() { return f() + 1; } }
namespace __attribute__((visibility("default"))) gnu { int h() { return 3; } }
namespace gnu::deeper { int k() { return h() + 1; } }
int main() { return outer::inner::g() == 2 && gnu::deeper::k() == 4 ? 0 : 1; }
