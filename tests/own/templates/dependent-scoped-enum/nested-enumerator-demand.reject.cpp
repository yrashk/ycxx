// [temp.inst] and [dcl.enum] require the inner enum definition when its enumerator is named.
template<int N> struct Inner {
  enum class Reading : unsigned char { good, bad = N }; //! error
};

template<int N> struct Outer {
  enum class Reading { from = static_cast<int>(Inner<N>::Reading::good) + 42 };
};

static_assert(static_cast<int>(Outer<-1>::Reading::from) == 42);
