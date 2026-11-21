namespace Flags {
template<class T> inline constexpr bool enabled = true;
}

template<bool B> struct Result { static constexpr bool value = B; };
template<bool B> using Identity = Result<B>;

namespace Consumer {
namespace Flags {
template<class T> inline constexpr bool enabled = false;
}

template<class T> using Direct = Identity<::Flags::enabled<T>>;
template<class T> using Forwarded = Direct<T>;
}

namespace Flags {
template<> inline constexpr bool enabled<long> = false;
template<class T> inline constexpr bool enabled<T*> = false;
}

static_assert(Consumer::Forwarded<int>::value);
static_assert(!Consumer::Forwarded<long>::value);
static_assert(!Consumer::Forwarded<int*>::value);
static_assert(__is_same(Consumer::Forwarded<long>, Result<false>));

namespace Flags {
template<class T> constexpr int number = 3;
template<> constexpr int number<char> = 7;
template<class T> constexpr bool lazy = T::value;
template<> constexpr bool lazy<int> = true;
}

template<int N> struct Number {};
template<class T> using NumberFor = Number<Flags::number<T>>;
static_assert(__is_same(NumberFor<int>, Number<3>));
static_assert(__is_same(NumberFor<char>, Number<7>));

template<class T> using Lazy = Result<Flags::lazy<T>>;
static_assert(Lazy<int>::value);

template<class T> struct Base : Consumer::Forwarded<T> {};
static_assert(Base<int>::value);
static_assert(!Base<long>::value);
