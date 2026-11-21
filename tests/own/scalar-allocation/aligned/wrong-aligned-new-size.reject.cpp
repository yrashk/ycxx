// [basic.stc.dynamic.allocation] The leading allocation parameter must have type size_t.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
void* operator new(unsigned int, std::align_val_t); //! error
