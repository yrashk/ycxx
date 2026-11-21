// [basic.stc.dynamic.allocation] Global allocation functions cannot inhabit a named namespace.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
namespace Wrong {
void* operator new(Count, std::align_val_t); //! error
}
