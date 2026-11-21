// [basic.stc.dynamic] Nonmember allocation and deallocation functions belong to global scope.
using Count = decltype(sizeof(0));
namespace Wrong {
  void* operator new[](Count); //! error
  void operator delete[](void*, Count) noexcept; //! error
}
