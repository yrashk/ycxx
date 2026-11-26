// The peer of mangling.run.cpp, compiled by the oracle: the definitions, explicitly instantiated.
template<auto X> struct cw {};
template<auto N> int f(cw<N>) { return N; }
template<auto N> int g() { return N; }
template<decltype(auto) N> int h() { return N; }
template<auto... N> int k() { return sizeof...(N); }
template<class T, auto X> int w(T t) { return X + t; }
template int f<7>(cw<7>);
template int g<3>();
template int g<'c'>();
template int h<4u>();
template int k<1, 'a'>();
template int k<>();
template int w<int, 2>(int);
