// [dcl.enum], [temp.inst]
template<long long N>
struct MemberEnum {
  enum E { first = N };
  using Underlying = __underlying_type(E);
};

using FitsInt = MemberEnum<1073741824LL>;
using ExceedsUnsignedInt = MemberEnum<8589934592LL>;

static_assert(static_cast<unsigned char>(-1) == 255);
static_assert(sizeof(int) == 4 && sizeof(long) == 8 && sizeof(void*) == 8);
static_assert(__is_same(FitsInt::Underlying, __underlying_type(FitsInt::E)));
static_assert(__is_same(ExceedsUnsignedInt::Underlying, __underlying_type(ExceedsUnsignedInt::E)));
static_assert(!__is_same(FitsInt::Underlying, ExceedsUnsignedInt::Underlying));
static_assert(sizeof(FitsInt::Underlying) == sizeof(int));
static_assert(sizeof(ExceedsUnsignedInt::Underlying) > sizeof(int));
static_assert(static_cast<FitsInt::Underlying>(FitsInt::first) == 1073741824LL);
static_assert(static_cast<ExceedsUnsignedInt::Underlying>(ExceedsUnsignedInt::first) == 8589934592LL);
