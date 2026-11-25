// #1314: complete groups retain constraint precedence ([temp.constr.normal]).
template<class T> void probe() noexcept(
    __is_nothrow_constructible(int, int&&, int&&) && __is_nothrow_assignable(T&, T&&));

template<class T> concept Inner = requires { probe<T>(); };
template<class T, class U> concept Pair = Inner<T> && __is_same(U, int);
template<class T> concept Direct = requires { requires (false && noexcept(probe<T>())); };
template<class T> concept Grouped = requires { requires ((false && Inner<T>)); };
template<class T> concept Precedence = requires { requires ((false && Pair<T, int>) || __is_same(T, void)); };
template<class T> concept WholeConjunction = requires { requires (true || Inner<T> && Inner<T>); };

static_assert(!Direct<void>);
static_assert(!Grouped<void>);
static_assert(Precedence<void>);
static_assert(WholeConjunction<void>);

// A root conditional or comma expression remains one atomic constraint.
template<class T> concept Conditional = requires { requires (false && __is_same(T, int) ? false : true); };
template<class T> concept Comma = requires { requires (false && __is_same(T, int), true); };
static_assert(Conditional<int>);
static_assert(Comma<int>);
