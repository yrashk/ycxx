// [dcl.array]: a substituted array bound must be representable in std::size_t.
// FLAGS: -Wno-pedantic
// XFAIL: gcc GCC 16 truncates the dependent bound to size_t.
using U = unsigned __int128;
// Clang points at the alias definition and ycxx points at its use.
template<U N> using A = int[N]; using TooWide = A<(U{1} << 64) + 1>; //! error
static_assert(sizeof(A<1>) == sizeof(int));
