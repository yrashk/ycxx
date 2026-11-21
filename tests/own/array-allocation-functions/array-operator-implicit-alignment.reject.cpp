// [basic.stc.dynamic] Neither integer nor a distinct scoped enum binds std::align_val_t.
using Count = decltype(sizeof(0));
namespace other { enum class align_val_t : Count {}; }

void reject(void* p, Count bytes, other::align_val_t alignment) {
  ::operator new[](bytes, 8); //! error
  ::operator delete[](p, bytes, 8); //! error
  ::operator new[](bytes, alignment); //! error
  ::operator delete[](p, bytes, alignment); //! error
}
