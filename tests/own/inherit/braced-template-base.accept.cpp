// [class.derived.general]/1, [temp.arg.general]/1, [expr.type.conv]/1
template<int N> struct Count { static constexpr int count = N; };
struct Element { using number = int; };

template<class T> struct Plain : Count<int{2}> {
  static constexpr int own = 3;
};

template<class T> struct Dependent : Count<T{4}> {};
template<class T> struct Qualified : Count<typename T::number{6}> {};

static_assert(Plain<Element>::count == 2);
static_assert(Plain<Element>::own == 3);
static_assert(Dependent<int>::count == 4);
static_assert(Qualified<Element>::count == 6);
