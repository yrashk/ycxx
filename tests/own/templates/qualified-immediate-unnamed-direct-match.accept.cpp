// [namespace.qual], [namespace.unnamed], [temp.arg.nontype]
template<unsigned Value> struct Constant { static constexpr unsigned value = Value; };
template<unsigned Value> using Base = Constant<Value>;

namespace Clock {
template<class T> consteval unsigned ticks() { return sizeof(T) + 7; }
namespace { struct ticks {}; }
}

template<class T> struct Result : Base<Clock::ticks<T>()> {};
static_assert(Result<char>::value == 8);
