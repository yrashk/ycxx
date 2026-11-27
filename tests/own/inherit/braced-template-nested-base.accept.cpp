// [class.derived.general]/1, [temp.arg.general]/1, [expr.type.conv]/1
template<int N> struct Count {};
template<class Wrapped> struct Wrap { static constexpr int marker = 7; };
template<class T> struct Nested : Wrap<Count<int{(8 >> 2)}>> {};
static_assert(Nested<int>::marker == 7);
