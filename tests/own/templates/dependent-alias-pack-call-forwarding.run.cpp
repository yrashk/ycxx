template<class... Ts> struct Sink {
    static int call(Ts&&... xs) {
        int value = 0;
        ((value = value * 10 + xs), ...);
        return value;
    }
};

template<class F, class... Ts> using Inv = Sink<Ts...>;

template<class F, class... Ts>
int pass(F&&, Ts&&... xs) {
    return Inv<F, Ts...>::call(static_cast<decltype(xs)&&>(xs)...);
}

int main() {
    if (pass('x') != 0) return 1;
    int first = 2;
    if (pass(17L, first, 3, 8) != 238) return 2;
    return 0;
}
