template<bool> using Ignore = int;
template<class T> using Erase = Ignore<__array_rank(T)>;
template<class T, class U = Erase<T>> struct Use {};
template<class> using Drop = int;

template<class T, class U = Drop<Use<T>>> struct Outer {};

namespace forwarding {
template<class T> using Forward = Drop<Use<T>>;
}

template<class T, class U = forwarding::Forward<T>> struct ForwardedOuter {};

using RankOne = int[2];
using RankTwo = int[2][3];
Outer<RankOne> direct_default;
ForwardedOuter<RankOne> forwarded_default;
Outer<RankTwo, int> direct_override;
ForwardedOuter<RankTwo, int> forwarded_override;

template<class T>
concept HasForwardedDefault = requires { typename ForwardedOuter<T>; };

static_assert(HasForwardedDefault<RankOne>);
static_assert(!HasForwardedDefault<RankTwo>);
