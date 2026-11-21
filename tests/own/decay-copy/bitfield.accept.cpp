struct Bits { unsigned int value : 3; };
Bits bits{};
template<class, class> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;
static_assert(same<decltype(+bits.value), int>);
static_assert(same<decltype(+auto(bits.value)), unsigned int>);
static_assert(same<decltype(+auto{bits.value}), unsigned int>);
