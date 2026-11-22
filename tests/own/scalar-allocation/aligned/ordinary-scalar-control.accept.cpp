// [expr.new], [expr.delete] Ordinary built-in scalar allocation remains available.
using Count = decltype(sizeof(0));
void* operator new(Count);
void operator delete(void*) noexcept;
void operator delete(void*, Count) noexcept;

int allocate_and_release() {
  int* p = new int(71);
  int value = *p;
  delete p;
  return value;
}
