// [dcl.enum], [temp.inst], [expr.alignof]
template<int N>
struct PrematureAlignment {
  enum E { first = N, alignment = alignof(E) }; //! error
};

static_assert(sizeof(PrematureAlignment<7>) > 0);
