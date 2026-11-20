// [temp.expl.spec], [temp.names]: grouped declarator IDs can contain nested template arguments.
namespace N {
using R = int;
template<class> struct Tag {};
template<class> struct C { static int (*p)(int); };
}

template<> N::R (*N::C<N::Tag<int>>::p)(int) = nullptr;

template<class A, class B> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;
using Callback = int (*)(int);
using Subject = N::C<N::Tag<int>>;
static_assert(same_type<decltype(Subject::p), Callback>);

int main() { return Subject::p == nullptr ? 0 : 1; }
