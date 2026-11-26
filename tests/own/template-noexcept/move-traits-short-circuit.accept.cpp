// A nested requirement's discarded operand needs no exception specification.
// [expr.prim.req.nested], [temp.constr.op], [except.spec]
template<class T> void probe() noexcept(
    __is_nothrow_constructible(int, int&&, int&&) && __is_nothrow_assignable(T&, T&&));
template<class T> void wrapped() noexcept(noexcept(probe<T>()));

template<class T> concept Conjunction = requires { requires false && noexcept(probe<T>()); };
template<class T> concept Disjunction = requires { requires true || noexcept(wrapped<T>()); };

static_assert(!Conjunction<void>);
static_assert(Disjunction<void>);

// Concept satisfaction and a selected helper's constraints run in their own analysis.
template<class T> concept Inner = requires { probe<T>(); };
template<class T> requires Inner<T> void constrained();
template<class T> constexpr bool indirectly() { return noexcept(probe<T>()); }

template<class T> concept Named = requires { requires false && Inner<T> || __is_same(T, void); };
template<class T> concept Associated = requires { requires false && requires { constrained<T>(); }; };
template<class T> concept Indirect = requires { requires __is_same(T, int) && indirectly<T>(); };
template<class T> concept WholeConjunction = requires { requires true || Inner<T> && requires { constrained<T>(); }; };

static_assert(Named<void>);
static_assert(!Associated<void>);
static_assert(!Indirect<void>);
static_assert(WholeConjunction<void>);
