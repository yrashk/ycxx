// [namespace.qual], [namespace.def], [temp.arg.nontype]
namespace Source {
template<class T> consteval unsigned ticks() { return sizeof(T) + 13; }
}

namespace Clock {
inline namespace Version { using namespace Source; }
}

template<unsigned Value> struct Constant { static constexpr unsigned value = Value; };
template<class T> struct Result : Constant<Clock::ticks<T>()> {};
static_assert(Result<char>::value == 14);
