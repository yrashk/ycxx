// XFAIL: gcc Implicitly instantiates the scoped member enum definition with the enclosing class [temp.inst]
// [temp.inst] and [dcl.enum] allow using the fixed enum type without its enumerators.
template<int Value> struct Register {
  enum class Reading : unsigned char { first = 0, second = Value };
};

static_assert(sizeof(Register<-1>) > 0);
using Reading = Register<-1>::Reading;
static_assert(sizeof(Reading) == sizeof(unsigned char));
