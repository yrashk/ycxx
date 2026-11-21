// Classify a namespace-scope unnamed enum through a dependent bool argument (#209).
template<bool> struct Kind {};

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

template<class T> using EnumKind = Kind<__is_enum(T)>;
template<class T> using Staged = EnumKind<T>;

enum { unnamed_enumerator };
using Unnamed = decltype(unnamed_enumerator);

static_assert(Same<Staged<Unnamed>, Kind<true>>::value);
static_assert(Same<Staged<Unnamed*>, Kind<false>>::value);
