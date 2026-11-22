// [dcl.enum], [temp.inst], [expr.sizeof]
template<int N>
struct PrematureSize {
  enum E { first = N, bytes = sizeof(E) }; //! error
};

static_assert(sizeof(PrematureSize<7>) > 0);
