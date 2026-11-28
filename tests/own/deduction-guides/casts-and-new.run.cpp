// #316: class template argument deduction in functional casts and new-expressions ([expr.type.conv]/1, [expr.new]/2)
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };

template <class T> struct Box {
    T v;
    Box(T x) : v(x) {}
};
template <class T> struct Ref {
    T* p;
    Ref(T& r) : p(&r) {}
};
template <class T> Ref(T&) -> Ref<T>;

int take(Box<long> b) { return int(b.v); }

int main() {
    auto a = Box(3);
    static_assert(same<decltype(a), Box<int>>::value);
    auto b = Box{4L};
    static_assert(same<decltype(b), Box<long>>::value);
    auto* p = new Box(5);
    static_assert(same<decltype(p), Box<int>*>::value);
    auto* q = new Box{'c'};
    static_assert(same<decltype(q), Box<char>*>::value);
    int n = 1;
    *Ref(n).p += 1;
    int r = a.v + int(b.v) + p->v + (q->v == 'c') + n + take(Box(6L));
    delete p;
    delete q;
    return r - 21;
}
