// [namespace.qual], [namespace.def], [temp.arg.nontype]
namespace Implementation {
template<class T, class Marker>
consteval int weight() { return sizeof(T) + Marker::value; }
}
namespace Source {
inline namespace Version { using namespace Implementation; }
}

template<bool Enabled> struct Flag { static constexpr int value = Enabled ? 12 : 0; };
template<int Value> struct Result { static constexpr int value = Value; };
template<class T> struct Check : Result<Source::weight<T, Flag<1 < 2>>()> {};
static_assert(Check<char>::value == 13);
