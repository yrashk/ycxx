// #316: explicit deduction-guides are candidates in direct- and direct-list-initialization, not in copy-initialization ([over.match.class.deduct], [over.match.copy])
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };

template <class T> struct E {
    E(T) {}
    template <class U> E(U*) {}
};
template <class T> explicit E(T*) -> E<T**>;

int i;
E direct(&i);
static_assert(same<decltype(direct), E<int**>>::value);
E list{&i};
static_assert(same<decltype(list), E<int**>>::value);
E copied = &i;
static_assert(same<decltype(copied), E<int*>>::value);
