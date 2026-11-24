// #257: failure to complete the field type propagates to its containing class.
template<int N> struct Bad {
  static_assert(N > 0); //! error
};

template<class T> struct Outer { T field; };

static_assert(!__is_empty(Outer<Bad<0>>));
