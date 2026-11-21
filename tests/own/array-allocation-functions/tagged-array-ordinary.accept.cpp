using Size = decltype(sizeof(0));
namespace std { enum class align_val_t : Size; }
struct Tag;
extern const Tag tag;
using Allocate = void* (*)(Size);

Allocate array_before() { return &::operator new[]; }
Allocate scalar_before() { return &::operator new; }

void* operator new[](Size, const Tag&) noexcept;
void* operator new[](Size, std::align_val_t, const Tag& = tag) noexcept;
void operator delete[](void*, const Tag&) noexcept;
void operator delete[](void*, std::align_val_t, const Tag&) noexcept;

Allocate array_after() { return &::operator new[]; }
Allocate scalar_after() { return &::operator new; }
void* array_bytes(Size size) { return ::operator new[](size); }
void* scalar_bytes(Size size) { return ::operator new(size); }
void release_array(void* p) { ::operator delete[](p); }
void release_scalar(void* p) { ::operator delete(p); }

int* make_array() { return new int[3]{}; }
int* make_scalar() { return new int(21); }
void drop_array(int* p) { delete[] p; }
void drop_scalar(int* p) { delete p; }
