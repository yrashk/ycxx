// [dcl.enum], [over.ics] An integer argument does not implicitly convert to the scoped enumeration.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
void* operator new(Count, std::align_val_t);
void operator delete(void*, Count, std::align_val_t) noexcept;
void* bad_allocate() { return ::operator new(sizeof(int), 8); } //! error
void bad_release(void* p) { ::operator delete(p, sizeof(int), 8); } //! error
