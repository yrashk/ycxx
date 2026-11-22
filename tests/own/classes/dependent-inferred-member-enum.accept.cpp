// [temp.dep.constexpr], [temp.inst], [dcl.enum]
template<long long N>
struct Inferred {
  enum State {
    first = N,
    second = first + 1,
    third
  };
};

using Small = Inferred<-4>;
using Wide = Inferred<8589934592LL>;

static_assert(Small::first == -4);
static_assert(Wide::second == 8589934593LL);
static_assert(Small::second == -3);
static_assert(Wide::third == 8589934594LL);
static_assert(Small::third == -2);
