// A nested requirement's discarded operand needs no exception specification.
// [expr.prim.req.nested], [temp.constr.op], [except.spec]
template<class T> void probe() noexcept(
    __is_nothrow_constructible(int, int&&, int&&) && __is_nothrow_assignable(T&, T&&));
template<class T> void wrapped() noexcept(noexcept(probe<T>()));

template<class T> concept Conjunction = requires { requires false && noexcept(probe<T>()); };
template<class T> concept Disjunction = requires { requires true || noexcept(wrapped<T>()); };

static_assert(!Conjunction<void>);
static_assert(Disjunction<void>);
