// [basic.stc.dynamic], [new.delete.single], [expr.new], [expr.delete]
// REQUIRES: linux
// PEER: aligned-scalar-provider.cpp
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
using Alignment = std::align_val_t;

void* allocate_before_redeclaration(Count count, Alignment alignment) {
  return ::operator new(count, alignment);
}

void release_before_redeclaration(void* address, Alignment alignment) {
  ::operator delete(address, alignment);
}

void* operator new(Count, std::align_val_t);
void operator delete(void*, std::align_val_t) noexcept;
void operator delete(void*, Count, std::align_val_t) noexcept;

extern "C" int check_ordinary_value(int* address);
extern "C" int aligned_scalar_status();

int main() {
  Alignment first_alignment = static_cast<Alignment>(alignof(unsigned long));
  void* first = allocate_before_redeclaration(17, first_alignment);
  release_before_redeclaration(first, first_alignment);

  Alignment second_alignment = static_cast<Alignment>(alignof(int));
  void* second = ::operator new(29, second_alignment);
  ::operator delete(second, 29, second_alignment);

  int* ordinary = new int(73);
  if (check_ordinary_value(ordinary) != 73) return 91;
  delete ordinary;
  return aligned_scalar_status();
}
