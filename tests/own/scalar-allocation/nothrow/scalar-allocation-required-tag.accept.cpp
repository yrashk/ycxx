using Size = decltype(sizeof(0));
struct Tag;
void* operator new(Size, const Tag&) noexcept;

int* allocate() { return new int(29); }
