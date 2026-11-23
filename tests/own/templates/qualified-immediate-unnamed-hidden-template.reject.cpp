// [namespace.qual], [namespace.unnamed], [temp.names]
namespace Clock {
consteval unsigned ticks() { return 4; }
namespace {
template<class T> consteval unsigned ticks() { return sizeof(T) + 7; }
}
}

template<unsigned> struct Constant {};
Constant<Clock::ticks<char>()> value; //! error
