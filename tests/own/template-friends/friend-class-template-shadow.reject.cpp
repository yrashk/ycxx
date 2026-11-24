// [temp.local]/6: a friend class template's parameters and name do not redeclare an enclosing
// template parameter; [dcl.type.elab]/2: `friend class T;` cannot name a template parameter.
template <class T>
struct A {
    template <class T> //! error
    friend class X;
};

template <class T>
struct B {
    template <class U>
    friend class T; //! error
};

template <class T>
struct C {
    friend class T; //! error
};

struct D {};
int main() {
    C<D> c;
    return 0;
}
