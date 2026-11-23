// [namespace.qual], [namespace.udecl], [namespace.unnamed]
namespace Source {
struct weight {};
namespace {
template<class T> consteval int weight() { return sizeof(T) + 5; }
}
}
namespace Imported { using Source::weight; }

template<int Value> struct Result {};
template<class T> struct Check : Result<Imported::weight<T>()> {}; //! error
