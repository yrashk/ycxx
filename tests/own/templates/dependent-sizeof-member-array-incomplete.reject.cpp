// REQUIRES: syntax
// [expr.sizeof], [temp.dep.constexpr], [temp.inst]
template<class T> struct Bytes {
  unsigned char data[sizeof(T)]; //! error
};

struct Incomplete;
Bytes<Incomplete>* declaration();
Bytes<Incomplete> forced;
