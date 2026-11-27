// #715: explicit(!same<T, int>) on a class template's constructor is evaluated per specialization; copy-initialization uses it only where it is false ([dcl.fct.spec], [over.match.copy])
// EXIT: 0
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;

template <class T> struct W {
    int tag;
    explicit(!same<T, int>) W(int) : tag(1) {}
    W(double) : tag(2) {}
};

int main() {
    W<int> i_copy = 1;
    if (i_copy.tag != 1) return 1;
    W<long> l_copy = 1;
    if (l_copy.tag != 2) return 2;
    W<long> l_direct(1);
    if (l_direct.tag != 1) return 3;
    W<long> l_list{1};
    if (l_list.tag != 1) return 4;
    return 0;
}
