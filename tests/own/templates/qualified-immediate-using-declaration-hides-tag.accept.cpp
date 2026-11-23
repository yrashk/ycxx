// [basic.lookup.general], [basic.lookup.qual], [namespace.udecl]
template<int N> struct Constant { static constexpr int value = N; };
template<int N> using Base = Constant<N>;

namespace Functions {
template<class T> consteval int probe() { return sizeof(T) + 5; }
}
namespace Scope {
struct probe {};
using Functions::probe;
}

template<class T> struct Derived : Base<Scope::probe<T>()> {};
static_assert(Derived<char>::value == 6);
