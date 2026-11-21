using Size = decltype(sizeof(0));
struct Tag;
extern const Tag tag;

int* before_declaration() { return new int[3]; }
void* operator new[](Size, const Tag&) noexcept;
int* before_default() { return new int[3]; }
void* operator new[](Size, const Tag& = tag) noexcept;
int* scalar_after_default() { return new int; }
int* array_after_default() { return new int[3]; } //! error
