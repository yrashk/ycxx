using Size = decltype(sizeof(0));
template<Size N> struct Value { static constexpr Size value = N; };
template<Size N> using Forward = Value<N>;
template<class T> struct Alignment : Forward<alignof(T)> {};
static_assert(Alignment<int>::value == alignof(int));
