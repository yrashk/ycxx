template<class... Ts> struct Sink { static void call(Ts&&...); };
template<class F, class... Ts> using Inv = Sink<Ts...>;

template<class F, class... Ts>
void pass(F&&, Ts&&... xs) { Inv<F, Ts...>::call(xs); } //! error
