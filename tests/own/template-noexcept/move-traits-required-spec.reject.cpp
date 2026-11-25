// Once needed, an exception specification's failed substitution is a hard error.
// The false construction trait does not make a reference to void well-formed.
template<class T> void probe() noexcept(
    __is_nothrow_constructible(int, int&&, int&&) && __is_nothrow_assignable(T&, T&&)); //! error

template<class T> concept Conjunction = requires { requires false && noexcept(probe<T>()); };
static_assert(!Conjunction<void>);

template<class T> concept Disjunction = requires { requires false || noexcept(probe<T>()); };
static_assert(!Disjunction<void>);
