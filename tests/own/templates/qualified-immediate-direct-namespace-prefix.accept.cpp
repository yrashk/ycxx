// [namespace.qual], [namespace.unnamed]
namespace Source {
namespace Inner {
template<class T> consteval int weight() { return sizeof(T) + 9; }
}
namespace {
namespace Inner {
template<class T> consteval int weight() { return sizeof(T) + 20; }
}
}
}

template<int Value> struct Result { static constexpr int value = Value; };
template<class T> struct Check : Result<Source::Inner::weight<T>()> {};
static_assert(Check<char>::value == 10);
