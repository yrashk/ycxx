// A specialization's base must be a complete class, not the specialization itself
// ([class.derived.general]/2) (#1134).
template<class T> struct P;
template<> struct P<int> : P<int> {}; //! error
template<class T> struct P<T*> : int {}; //! error
union U {};
template<class T> union V;
template<> union V<int> : U {}; //! error
