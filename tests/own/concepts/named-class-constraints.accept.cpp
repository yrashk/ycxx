template<class T> concept IntType = __is_same(T, int);
template<IntType T> struct Named;
Named<int>* accepted;
