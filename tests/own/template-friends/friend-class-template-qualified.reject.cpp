// [dcl.meaning.general]/2.3, [temp.friend]/1: a qualified friend class template declaration names a
// class template the namespace declared earlier; it declares none.
namespace n {
template <class T> class Here;
}

class A {
    template <class T>
    friend class n::Missing; //! error
};

class B {
    template <class T>
    friend class n::Here;
};
