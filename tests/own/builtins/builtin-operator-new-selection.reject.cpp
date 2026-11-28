// [new.delete.single]: __builtin_operator_new and __builtin_operator_delete must select a
// replaceable global allocation or deallocation function (docs/EXTENSIONS.md): a usual one or
// its std::nothrow_t form, never a placement or other written overload.
using Count = decltype(sizeof(0));
namespace std {
  enum class align_val_t : Count {};
  struct nothrow_t { explicit nothrow_t() = default; };
  extern const nothrow_t nothrow;
}
namespace other { struct nothrow_t {}; }
void* operator new(Count, void*) noexcept;
void* operator new(Count, const other::nothrow_t&) noexcept;
void* operator new(Count, const std::nothrow_t&) noexcept;
void operator delete(void*, const std::nothrow_t&) noexcept;

void* storage;
void* placed = __builtin_operator_new(8, storage); //! error
void* tagged = __builtin_operator_new(4, 1); //! error
void* foreign = __builtin_operator_new(4, other::nothrow_t{}); //! error
void* none = __builtin_operator_new(); //! error
void* fine = __builtin_operator_new(4, std::nothrow);
void fine_delete() { __builtin_operator_delete(fine, std::nothrow); }
void empty_delete() { __builtin_operator_delete(); } //! error
void wrong_delete(int* p) { __builtin_operator_delete(p, 1, 2, 3); } //! error
template <class T> void* later(T t) { return __builtin_operator_new(4, t); } //! error
void* instantiated = later(1);
void* aligned = later(std::align_val_t(16));
auto address = &__builtin_operator_new; //! error
