// [basic.lookup.qual], [namespace.memdef], [temp.arg.nontype]
namespace Measures {
template<class T> consteval int weight();
}
template<class T> consteval int Measures::weight() { return 15 + sizeof(T); }

using Measures::weight;

template<int Value> struct Result { static constexpr int value = Value; };
template<class T> struct Check : Result<::weight<T>()> {};
static_assert(Check<char>::value == 16);
