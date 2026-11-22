// [basic.stc.dynamic] Nonmember allocation and deallocation functions belong to global scope.
using Count = decltype(sizeof(0));
struct Token;

namespace Wrong {
  void* operator new(Count, const Token&) noexcept; //! error
  void operator delete(void*, const Token&) noexcept; //! error
}
