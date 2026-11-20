// An inline namespace member can be defined using its enclosing namespace ([namespace.def]).
int helper(int) { return 1; }
namespace n {
inline namespace abi {
int helper(int x) { return x + 2; }
extern "C" int ycxx_inline_owner(int);
}
}
int n::ycxx_inline_owner(int x) { return helper(x); }
int main() { return n::ycxx_inline_owner(40) - 42; }
