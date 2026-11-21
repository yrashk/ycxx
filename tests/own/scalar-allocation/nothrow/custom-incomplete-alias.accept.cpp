// [dcl.link], [except.spec] A named incomplete class suffices for the const reference parameter.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
namespace controls { struct Token; }
using TagRef = const controls::Token&;
using Alignment = std::align_val_t;

extern "C++" {
  void* operator new(Count, TagRef) noexcept;
  void* operator new(Count, Alignment, TagRef) noexcept(true);
  void operator delete(void*, TagRef);
  void operator delete(void*, Alignment, TagRef) noexcept;
}

void* operator new(Count, const controls::Token&) noexcept;

void use_written(Alignment alignment, TagRef tag) {
  Count size = sizeof(int);
  void* unaligned = ::operator new(size, tag);
  ::operator delete(unaligned, tag);
  void* aligned = ::operator new(size, alignment, tag);
  ::operator delete(aligned, alignment, tag);
}
