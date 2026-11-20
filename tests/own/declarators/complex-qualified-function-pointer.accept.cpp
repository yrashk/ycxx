// [dcl.meaning.general], [dcl.ptr], [dcl.fct]: qualified IDs and nested pointer parameters.
namespace N {
using R = int;
using Unary = int (*)(int);
using Higher = int (*)(Unary);
extern R (*p)(int);
}

N::R (*N::p)(int) = nullptr;
using N::p;

template<class A, class B> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;
static_assert(same_type<decltype(p), N::Unary>);

void f() {
  N::R (*p)(int (*)(int));
  static_assert(same_type<decltype(p), N::Higher>);
}

int main() { return p == nullptr ? 0 : 1; }
