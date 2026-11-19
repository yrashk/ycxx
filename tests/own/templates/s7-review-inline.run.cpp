// ζ s7 (#88), from the step's review: inline static members, constant or not, have one object per specialization
// ([class.static.data]/4).
// EXIT: 0
template<class T> struct S {
  inline static int counter = 0;
  inline static int init = sizeof(T);
  static int bump() { return ++counter; }
};
int main() { S<int>::bump(); S<int>::bump(); S<char>::bump();
  return S<int>::counter == 2 && S<char>::counter == 1 && S<int>::init == 4 && S<char>::init == 1 ? 0 : 1; }
