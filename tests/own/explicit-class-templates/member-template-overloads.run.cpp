// #715: copy-initialization skips an explicit constructor template and uses a converting constructor; direct-initialization picks the exact template specialization ([over.match.copy], [over.match.funcs.general])
// EXIT: 0
struct S {
    int tag;
    template <class U> explicit S(U) : tag(1) {}
    S(long) : tag(2) {}
};

template <class T> struct W {
    int tag;
    template <class U> explicit W(U) : tag(1) {}
    W(long) : tag(2) {}
};

int take(W<char> w) { return w.tag; }

int main() {
    S s_direct(1);
    if (s_direct.tag != 1) return 1;
    S s_copied = 1;
    if (s_copied.tag != 2) return 2;
    W<char> w_direct{1};
    if (w_direct.tag != 1) return 3;
    W<char> w_copied = 1;
    if (w_copied.tag != 2) return 4;
    if (take(1) != 2) return 5;
    return 0;
}
