// [basic.stc.dynamic] The six usual array overloads are visible before written declarations.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
using Alignment = std::align_val_t;

void before_redeclaration(Count bytes, Alignment alignment) {
  void* plain = ::operator new[](bytes);
  ::operator delete[](plain);
  void* sized = operator new[](bytes);
  operator delete[](sized, bytes);
  void* aligned = ::operator new[](bytes, alignment);
  ::operator delete[](aligned, alignment);
  void* sized_aligned = operator new[](bytes, alignment);
  operator delete[](sized_aligned, bytes, alignment);
}

void* operator new[](Count);
void* operator new[](Count, Alignment);
void operator delete[](void*) noexcept;
void operator delete[](void*, Count) noexcept;
void operator delete[](void*, Alignment) noexcept;
void operator delete[](void*, Count, Alignment) noexcept;

void after_redeclaration(Count bytes, Alignment alignment) {
  void* plain = operator new[](bytes);
  operator delete[](plain);
  void* sized = ::operator new[](bytes);
  ::operator delete[](sized, bytes);
  void* aligned = operator new[](bytes, alignment);
  operator delete[](aligned, alignment);
  void* sized_aligned = ::operator new[](bytes, alignment);
  ::operator delete[](sized_aligned, bytes, alignment);
}
