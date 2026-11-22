// #234 item 2: two partial specializations with the same arguments whose constraints do not
// subsume each other are both declared; a specialization they both match is ambiguous
// ([temp.spec.partial.match]/1.2, [temp.constr.order]).
template<class T> concept Four = sizeof(T) == 4;
template<class T> concept Aligned = alignof(T) == 4;
template<class T> struct R { static constexpr int v = 1; };
template<class T> requires Four<T> struct R<T> { static constexpr int v = 2; };
template<class T> requires Aligned<T> struct R<T> { static constexpr int v = 3; };
int a = R<int>::v; //! error
int b = R<char>::v;
