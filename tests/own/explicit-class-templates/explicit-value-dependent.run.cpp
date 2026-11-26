// #715: after deduction an explicit specialization is removed from copy-initialization, which then falls back to a converting constructor; direct-initialization keeps it ([over.match.funcs.general], [over.match.copy])
// EXIT: 0
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;

template <class T> struct Opt {
    int tag;
    template <class U> explicit(!same<U, T>) Opt(U) : tag(1) {}
    Opt(double) : tag(2) {}
};

struct S {
    int tag;
    template <class U> explicit(!same<U, int>) S(U) : tag(1) {}
    S(double) : tag(2) {}
};

int take(Opt<int> o) { return o.tag; }
Opt<int> give() { return 1L; }

int main() {
    Opt<int> same_copy = 1;
    if (same_copy.tag != 1) return 1;
    Opt<int> other_copy = 1L;
    if (other_copy.tag != 2) return 2;
    Opt<int> other_direct(1L);
    if (other_direct.tag != 1) return 3;
    if (take(1) != 1) return 4;
    if (take(1L) != 2) return 5;
    if (give().tag != 2) return 6;
    S s_same = 1;
    if (s_same.tag != 1) return 7;
    S s_other = 'c';
    if (s_other.tag != 2) return 8;
    S s_direct('c');
    if (s_direct.tag != 1) return 9;
    return 0;
}
