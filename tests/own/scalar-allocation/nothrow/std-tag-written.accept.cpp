// [new.delete.single] These four written declarations add the scalar nothrow overloads.
using Count = decltype(sizeof(0));
namespace std {
  enum class align_val_t : Count {};
  struct nothrow_t;
}

void* operator new(Count, const std::nothrow_t&) noexcept;
void* operator new(Count, std::align_val_t, const std::nothrow_t&) noexcept;
void operator delete(void*, const std::nothrow_t&) noexcept;
void operator delete(void*, std::align_val_t, const std::nothrow_t&) noexcept;

void use_written(std::align_val_t alignment, const std::nothrow_t& tag) {
  Count size = sizeof(int);
  void* unaligned = ::operator new(size, tag);
  ::operator delete(unaligned, tag);
  void* aligned = ::operator new(size, alignment, tag);
  ::operator delete(aligned, alignment, tag);
}
