// [basic.stc.dynamic], [new.delete.array]
// PEER: array-direct-provider.cpp
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
using Alignment = std::align_val_t;

void before_redeclaration() {
  void* plain = ::operator new[](17);
  ::operator delete[](plain);

  Alignment alignment = static_cast<Alignment>(alignof(unsigned long));
  void* aligned = operator new[](23, alignment);
  operator delete[](aligned, alignment);
}

void* operator new[](Count);
void* operator new[](Count, Alignment);
void operator delete[](void*) noexcept;
void operator delete[](void*, Count) noexcept;
void operator delete[](void*, Alignment) noexcept;
void operator delete[](void*, Count, Alignment) noexcept;

extern "C" int array_direct_status();

int main() {
  before_redeclaration();

  Count size = 29;
  void* sized = ::operator new[](size);
  ::operator delete[](sized, size);

  size = 31;
  Alignment alignment = static_cast<Alignment>(alignof(int));
  void* sized_aligned = ::operator new[](size, alignment);
  ::operator delete[](sized_aligned, size, alignment);

  size = 37;
  void* scalar = ::operator new(size);
  ::operator delete(scalar, size);

  size = 41;
  void* aligned_scalar = ::operator new(size, alignment);
  ::operator delete(aligned_scalar, size, alignment);
  return array_direct_status();
}
