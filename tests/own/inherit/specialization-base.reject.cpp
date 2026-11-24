// A specialization's base must be a complete class, not the specialization itself
// ([class.derived.general]/2) (#1134).
template<class T> struct P;
template<> struct P<int> : P<int> {}; //! error
template<class T> struct P<T*> : int {}; //! error
union U {};
template<class T> union V;
template<> union V<int> : U {}; //! error
template<class T> struct R;
template<class T> struct R<T*> : R<int*> {}; //! error
struct B { int u = 1; };
template<> class P<long> : B {};
int private_base() { return P<long>{}.u; } //! error
template<class T> class P<T&> : B {};
int private_partial_base() { return P<int&>{}.u; } //! error
