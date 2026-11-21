using Size = decltype(sizeof(0));
struct Tag;
extern const Tag tag;

int* before_declaration() { return new int; }
void* operator new(Size, const Tag&) noexcept;
int* before_default() { return new int; }
void* operator new(Size, const Tag& = tag) noexcept;
int* after_default() { return new int; } //! error
