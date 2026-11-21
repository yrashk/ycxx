// [basic.stc.dynamic], [new.syn] Usual aligned scalar declarations share the declared scoped enum.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
using Alignment = std::align_val_t;
using Allocate = void* (*)(Count, Alignment);
using Release = void (*)(void*, Alignment) noexcept;
using ReleaseSized = void (*)(void*, Count, Alignment) noexcept;

void before_redeclaration(void* p, Alignment alignment) {
  Allocate allocate = &::operator new;
  Release release = &::operator delete;
  ReleaseSized release_sized = &::operator delete;
  void* storage = allocate(sizeof(int), alignment);
  release(storage, alignment);
  release_sized(p, sizeof(int), alignment);
}

void* operator new(Count, std::align_val_t);
void operator delete(void*, std::align_val_t) noexcept;
void operator delete(void*, Count, std::align_val_t) noexcept;

void after_redeclaration(void* p, Alignment alignment) {
  Allocate allocate = &::operator new;
  Release release = &::operator delete;
  ReleaseSized release_sized = &::operator delete;
  void* storage = allocate(sizeof(int), alignment);
  release(storage, alignment);
  release_sized(p, sizeof(int), alignment);
}
