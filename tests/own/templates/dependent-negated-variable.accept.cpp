namespace Outer {
template<class T> inline constexpr bool flag = false;
template<> inline constexpr bool flag<int> = true;
}

template<bool Value> struct Record {
    static constexpr bool value = Value;
};

template<class T> using Direct = Record<::Outer::flag<T>>;
template<class T> using Negated = Record<!::Outer::flag<T>>;
template<class T> using Staged = Negated<T>;

template<class T, bool Value = !::Outer::flag<T>>
using Defaulted = Record<Value>;

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

static_assert(Direct<int>::value);
static_assert(!Direct<long>::value);
static_assert(Same<Negated<int>, Record<false>>::value);
static_assert(Same<Negated<long>, Record<true>>::value);
static_assert(Same<Staged<int>, Direct<long>>::value);
static_assert(Same<Staged<long>, Direct<int>>::value);
static_assert(Same<Defaulted<int>, Record<false>>::value);
static_assert(Same<Defaulted<long>, Record<true>>::value);
static_assert(Same<Defaulted<int, true>, Record<true>>::value);
static_assert(Same<Defaulted<long, false>, Record<false>>::value);
