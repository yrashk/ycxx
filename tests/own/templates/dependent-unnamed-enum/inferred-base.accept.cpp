// [temp.dep.constexpr], [dcl.enum]
template<long long Start> struct Offset {
  static constexpr long long base = Start;
  enum { first = base, second, last = second + 2 };
};

using Low = decltype(Offset<-2>::first);
using High = decltype(Offset<0x100000000LL>::first);
static_assert(Offset<-2>::first == -2);
static_assert(Offset<-2>::last == 1);
static_assert(Offset<0x100000000LL>::second == 0x100000001LL);
static_assert(Offset<0x100000000LL>::last == 0x100000003LL);
static_assert(sizeof(Low) <= sizeof(int));
static_assert(static_cast<__underlying_type(High)>(0x100000003ULL) == 0x100000003ULL);
