// [basic.stc.dynamic] Neither the tag's name nor a later declaration adds an earlier overload.
using Count = decltype(sizeof(0));
namespace std {
  enum class align_val_t : Count {};
  struct nothrow_t;
}

void before_written(void* pointer, std::align_val_t alignment, const std::nothrow_t& tag) {
  Count size = sizeof(int);
  ::operator new(size, tag); //! error
  ::operator new(size, alignment, tag); //! error
  ::operator delete(pointer, tag); //! error
  ::operator delete(pointer, alignment, tag); //! error
}

void* operator new(Count, const std::nothrow_t&) noexcept;
void* operator new(Count, std::align_val_t, const std::nothrow_t&) noexcept;
void operator delete(void*, const std::nothrow_t&) noexcept;
void operator delete(void*, std::align_val_t, const std::nothrow_t&) noexcept;
