// #316: copy-list-initialization that selects an explicit deduction-guide is ill-formed ([over.match.class.deduct], [over.match.list])
template <class T> struct A {
    A(T, T) {}
};
template <class T> explicit A(T, T) -> A<T>;

A direct{0, 1};
A copy_list = {0, 1}; //! error
