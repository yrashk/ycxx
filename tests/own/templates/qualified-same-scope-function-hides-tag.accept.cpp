// [basic.lookup.general], [basic.lookup.qual], [temp.arg.nontype]
template<int V> struct Constant { static constexpr int value = V; };
template<int V> using Base = Constant<V>;

namespace Lookup {
struct probe {};
template<class T> consteval int probe() { return sizeof(T); }
}

template<class T> struct Derived : Base<Lookup::probe<T>()> {};
static_assert(Derived<char>::value == 1);
