using Size = decltype(sizeof(0));
struct Tag;
extern const Tag tag;
void* operator new(Size, const Tag& = tag) noexcept;

int* allocate() { return new int; } //! error
