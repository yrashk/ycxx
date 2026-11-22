template<class... Ts> struct Sink { static void call(Ts&&...); };
template<class... Ts> using Inv = Sink<Ts...>;

template<class... Ts>
void direct(Ts&&... xs) { Sink<Ts...>::call(xs...); }

template<class... Ts>
void alias(Ts&&... xs) { Inv<Ts...>::call(xs...); }
