template<__SIZE_TYPE__> using Ignore = int;

template<class T> requires (__is_array(T))
using Erase = Ignore<__array_rank(T)>;

template<class T, class U = Erase<T>> struct Use {};

Use<int[2][3]> omitted;
Use<int, int> explicit_int;

template<class T>
concept HasDefault = requires { typename Use<T>; };

static_assert(HasDefault<int[2][3]>);
static_assert(!HasDefault<int>);
