// [basic.stc.dynamic], [new.delete.single], [expr.new], [expr.delete]
// PEER: tagged-scalar-provider.cpp
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
namespace fixture { struct Token { int key; }; }
using Tag = fixture::Token;
using Alignment = std::align_val_t;

extern "C++" {
  void* operator new(Count, const Tag&) noexcept;
  void* operator new(Count, Alignment, const Tag&) noexcept;
  void operator delete(void*, const Tag&);
  void operator delete(void*, Alignment, const Tag&) noexcept;
}

extern "C" int inspect_ordinary(int*);
extern "C" int tagged_probe_status();

int main() {
  Tag first{13};
  void* storage = ::operator new(17, first);
  ::operator delete(storage, first);

  Alignment alignment = static_cast<Alignment>(alignof(unsigned long));
  Tag second{29};
  void* aligned = ::operator new(23, alignment, second);
  ::operator delete(aligned, alignment, second);

  int* ordinary = new int(73);
  if (inspect_ordinary(ordinary) != 73) return 91;
  delete ordinary;
  return tagged_probe_status();
}
