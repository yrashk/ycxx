// Functional conversions of namespace-qualified class and alias template-ids ([expr.type.conv]).
namespace Effects {
template<class T> struct Tag {};
template<class T> using TagAlias = Tag<T>;
template<class T> constexpr int function() { return sizeof(T); }
template<class T> constexpr int variable = sizeof(T);
}

namespace Alias = Effects;
auto direct = Effects::Tag<int>{};
auto through_alias = Alias::Tag<int>{};
static_assert(__is_same(decltype(direct), Effects::Tag<int>));
static_assert(__is_same(decltype(through_alias), decltype(::Effects::Tag<int>())));
static_assert(__is_same(decltype(Alias::TagAlias<int>{}), Effects::Tag<int>));
static_assert(__is_same(decltype(Effects::Tag<Alias::Tag<int>>{}), Effects::Tag<Effects::Tag<int>>));
static_assert(Effects::function<int>() == sizeof(int));
static_assert(Alias::variable<int> == sizeof(int));
