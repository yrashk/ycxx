// [basic.lookup.qual], [namespace.memdef], [temp.arg.nontype]
namespace Measures {
template<class T> consteval int weight();
}
template<class T> consteval int Measures::weight() { return 15 + sizeof(T); }

template<int Value> struct Result { static constexpr int value = Value; };
template<class T> struct Check : Result<Measures::weight<T>()> {};
static_assert(Check<char>::value == 16);
