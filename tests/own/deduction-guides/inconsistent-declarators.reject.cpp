// #316: the declarators of one declaration deduce the same class template specialization ([dcl.type.class.deduct]/1, [dcl.type.auto.deduct]/2)
template <class T> struct Box {
    Box(T) {}
};

Box a(1), b(2); // the same Box<int>
Box c(1), d(2.0); //! error

void f() {
    Box e(1), g(2);
    Box h(1), k(2.0); //! error
}
