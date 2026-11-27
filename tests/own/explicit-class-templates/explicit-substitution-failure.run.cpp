// #715: the explicit-specifier is substituted during deduction; explicit(U::value) with U = int fails deduction and removes the candidate ([temp.deduct.general], [over.match.funcs.general])
// EXIT: 0
struct Off { static constexpr bool value = false; };
struct On { static constexpr bool value = true; };

struct S {
    int tag;
    template <class U> explicit(U::value) S(U) : tag(1) {}
    S(double) : tag(2) {}
};

template <class T> struct W {
    int tag;
    template <class U> explicit(U::value) W(U) : tag(1) {}
    W(double) : tag(2) {}
};

int main() {
    S s_int(1);
    if (s_int.tag != 2) return 1;
    S s_int_copy = 1;
    if (s_int_copy.tag != 2) return 2;
    S s_off = Off{};
    if (s_off.tag != 1) return 3;
    S s_on{On{}};
    if (s_on.tag != 1) return 4;
    W<int> w_int{1};
    if (w_int.tag != 2) return 5;
    W<int> w_off = Off{};
    if (w_off.tag != 1) return 6;
    W<int> w_on(On{});
    if (w_on.tag != 1) return 7;
    return 0;
}
