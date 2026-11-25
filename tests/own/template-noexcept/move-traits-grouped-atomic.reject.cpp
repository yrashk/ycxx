// An outer equality makes this an atomic constraint ([temp.constr.normal]).
// Both operands of its ordinary nested && are formed, so probe<void> needs its specification.
template<class T> void probe() noexcept(
    __is_nothrow_constructible(int, int&&, int&&) && __is_nothrow_assignable(T&, T&&)); //! error

template<class T> concept Atomic = requires { requires ((false && noexcept(probe<T>())) == false); };
static_assert(Atomic<void>);
