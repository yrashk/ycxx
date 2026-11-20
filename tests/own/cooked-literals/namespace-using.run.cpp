// [lex.ext], [namespace.udecl]: namespace using-declarations and qualified explicit calls.
// EXIT: 0
namespace A {
constexpr unsigned long long operator""_k(unsigned long long v) {
  return v + 7;
}
}
using A::operator "" _k;
static_assert(3_k == 10);
static_assert(A::operator""_k(35ULL) == 42);
int main() { return 3_k + A::operator""_k(25ULL) == 42 ? 0 : 1; }
