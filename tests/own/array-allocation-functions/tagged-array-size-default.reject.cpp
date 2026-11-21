using Size = decltype(sizeof(0));
struct Tag;
extern const Tag tag;

void* operator new[](Size = sizeof(int), const Tag& = tag) noexcept; //! error
