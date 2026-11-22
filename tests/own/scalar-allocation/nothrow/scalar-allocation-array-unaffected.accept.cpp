using Size = decltype(sizeof(0));
struct Tag;
extern const Tag tag;
void* operator new(Size, const Tag& = tag) noexcept;

using Allocation = void* (*)(Size);
Allocation array_address() { return &::operator new[]; }
void* allocate_array_bytes() { return ::operator new[](sizeof(int) * 3); }
int* allocate_array() { return new int[3]{}; }
void release_array(int* p) { delete[] p; }
