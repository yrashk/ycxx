namespace Outer {
template<class T> inline constexpr bool flag = false;
template<> inline constexpr bool flag<int> = true;
}

template<bool Value> struct Record {
    static constexpr bool value = Value;
};

template<class T> using Negated = Record<!::Outer::flag<T>>;

static_assert(Negated<long>::value);
static_assert(Negated<int>::value); //! error
