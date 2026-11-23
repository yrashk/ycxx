// #209 [temp.arg.nontype], [temp.variadic], [temp.alias].
namespace Check {
template<class T, class... Args>
concept Fits = sizeof...(Args) == 0;
}

template<bool Result>
struct Bool {};

template<bool Result>
using Wrap = Bool<Result>;

template<class T, class... Args>
struct Record : Wrap<Check::Fits<T, Args...>> {};

static_assert(__is_base_of(Bool<true>, Record<char>));
static_assert(__is_base_of(Bool<false>, Record<char, int>));
