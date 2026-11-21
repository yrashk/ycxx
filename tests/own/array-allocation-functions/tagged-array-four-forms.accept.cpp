using Size = decltype(sizeof(0));
namespace std { enum class align_val_t : Size; }
struct Tag;
using TagRef = const Tag&;

void* operator new[](Size, TagRef) noexcept;
void* operator new[](Size, std::align_val_t, TagRef) noexcept;
void operator delete[](void*, TagRef);
void operator delete[](void*, std::align_val_t, TagRef) noexcept;

using Allocate = void* (*)(Size, TagRef);
using AllocateAligned = void* (*)(Size, std::align_val_t, TagRef);
using Release = void (*)(void*, TagRef);
using ReleaseAligned = void (*)(void*, std::align_val_t, TagRef);

Allocate allocate_address() { return &::operator new[]; }
AllocateAligned aligned_address() { return &::operator new[]; }
Release release_address() { return &::operator delete[]; }
ReleaseAligned aligned_release_address() { return &::operator delete[]; }

void* allocate(Size size, TagRef tag) { return ::operator new[](size, tag); }
void* allocate(Size size, std::align_val_t alignment, TagRef tag) {
  return ::operator new[](size, alignment, tag);
}
void release(void* pointer, TagRef tag) { ::operator delete[](pointer, tag); }
void release(void* pointer, std::align_val_t alignment, TagRef tag) {
  ::operator delete[](pointer, alignment, tag);
}
