// [basic.stc.dynamic], [new.syn] The aligned scalar overloads use the declared scoped enum.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
using Alignment = std::align_val_t;
using Allocate = void* (*)(Count, Alignment);

void before_redeclaration(Alignment alignment) {
  Allocate allocate = &::operator new;
  Count size = sizeof(int);
  void* first = allocate(size, alignment);
  ::operator delete(first, alignment);
  void* second = allocate(size, alignment);
  ::operator delete(second, size, alignment);
}

void* operator new(Count, std::align_val_t);
void operator delete(void*, std::align_val_t) noexcept;
void operator delete(void*, Count, std::align_val_t) noexcept;

void after_redeclaration(Alignment alignment) {
  Allocate allocate = &::operator new;
  Count size = sizeof(int);
  void* first = allocate(size, alignment);
  ::operator delete(first, alignment);
  void* second = allocate(size, alignment);
  ::operator delete(second, size, alignment);
}
