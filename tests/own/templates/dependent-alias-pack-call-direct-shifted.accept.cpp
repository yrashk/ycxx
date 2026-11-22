template<class... Ts> struct Sink { static void invoke(Ts&&...); };

template<class F, class... Ts>
void call(F&&, Ts&&... xs) { Sink<Ts...>::invoke(xs...); }
