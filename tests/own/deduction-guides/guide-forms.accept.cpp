// #316: deduction-guide declarations in the forms the standard library uses parse at namespace scope and in a namespace ([temp.deduct.guide])
namespace n {
template <class T> struct remove_ref { using type = T; };
template <class T> struct remove_ref<T&> { using type = T; };
template <class T> struct V {
    V(T) {}
    template <class I> V(I, I) {}
};
template <class T, class U = int> struct Two {
    Two(T, U) {}
};
template <class T> V(T*, T*) -> V<T>;
template <class T>
    requires(sizeof(T) > 1)
V(T, int) -> V<typename remove_ref<T>::type>;
template <class T> explicit(sizeof(T) == 1) V(T&) -> V<T>;
template <class T, class U = long> Two(T, U = U()) -> Two<T, U>;
template <class... Ts> struct Many {
    Many(Ts...) {}
};
template <class... Ts> Many(Ts...) -> Many<Ts...>;
template <class T, int N> struct Arr {
    T e[N];
};
template <class T, int N> Arr(const T (&)[N]) -> Arr<T, N>;
Two(const char*) -> Two<const char*, int>;
}  // namespace n
