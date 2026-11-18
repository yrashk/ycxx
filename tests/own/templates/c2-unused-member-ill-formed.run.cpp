// ζ c2 (#88): instantiating a class template specialization instantiates the declarations of its
// member functions but not their definitions; a definition is instantiated only when the member
// is used. So members whose bodies are ill-formed for this argument (in the class, outside it,
// static) are harmless as long as they are never called ([temp.inst]/3, [temp.inst]/5,
// [temp.res]).
// EXIT: 0
struct Plain {
  int n;
};

template<class T>
struct Box {
  T v;
  int get_n() const { return v.n; }
  int length() const { return v.size(); }
  static int zero() { return T::zero_value; }
  int twice() const;
};

template<class T>
int Box<T>::twice() const { return v + v; }

int main() {
  Box<int> a{4};
  if (a.twice() != 8) return 1;
  Box<Plain> p{{3}};
  if (p.get_n() != 3) return 2;
  Box<long> l{-1L};
  if (l.v != -1L) return 3;
  return 0;
}
