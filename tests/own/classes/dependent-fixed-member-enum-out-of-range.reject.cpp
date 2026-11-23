// [dcl.enum], [expr.const.const], [temp.inst]
template<int N>
struct Checked {
  enum State : unsigned char { first = N }; //! error
};

static_assert(Checked<1>::first == 1);
static_assert(sizeof(Checked<-1>) > 0);
