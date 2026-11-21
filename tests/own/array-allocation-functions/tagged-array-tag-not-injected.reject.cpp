using Size = decltype(sizeof(0));
struct Tag;

void* allocate(Size size, const Tag& tag) { return ::operator new[](size, tag); } //! error
