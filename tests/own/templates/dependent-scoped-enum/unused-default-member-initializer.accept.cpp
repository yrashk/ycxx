// XFAIL: gcc Implicitly instantiates the scoped member enum definition with the enclosing class [temp.inst]
// [temp.inst] leaves the default member initializer uninstantiated for sizeof.
template<int N> struct Sample {
  enum class Reading : unsigned char { first, bad = N };
  Reading field = Reading::first;
};

static_assert(sizeof(Sample<-1>) > 0);
