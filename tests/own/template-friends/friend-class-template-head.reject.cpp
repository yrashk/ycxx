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
