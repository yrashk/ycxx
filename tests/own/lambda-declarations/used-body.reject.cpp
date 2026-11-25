// The body of a lambda in a used static data member of a class template is instantiated with it.
template <class T>
struct W {
  static constexpr int m = [] { return T::nope; }();  //! error
};
int x = W<int>::m;
