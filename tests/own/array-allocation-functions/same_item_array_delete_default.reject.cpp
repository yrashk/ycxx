struct Tag;
extern const Tag tag;

void operator delete[](void*, const Tag& = tag) noexcept,
    *p = (::operator delete[](nullptr), nullptr); //! error
