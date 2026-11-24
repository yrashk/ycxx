// [temp.over.link]/6, [basic.scope.scope]/4: a friend class template declaration redeclares the
// class template of its namespace, so their template-heads must agree; [temp.param]/12: it gives
// no default template arguments. Each case is on one line, where both oracles report it.
template <class T> class F; class A { template <int N> friend class F; }; //! error
template <class T, int N> class G; class B { template <class T, long N> friend class G; }; //! error
class C { template <class T = int> friend class H; }; //! error

// Heads that agree, whatever the parameters' names.
template <class T, int N, template <class> class P> class K;
class D {
    template <class U, int M, template <class> class Q>
    friend class K;
};

// Dependent parameter types compare after pairing each head's own bound positions.
template<class T, int N> class Fixed; class BadFixed { template<class T, T N> friend class Fixed; }; //! error
template<class A, class B, A N> class Pair; class BadPair { template<class A, class B, B N> friend class Pair; }; //! error
template<class T, T N> class Own; template<class Outer> class BadFree { template<class T, Outer N> friend class Own; }; BadFree<int> bad; //! error
