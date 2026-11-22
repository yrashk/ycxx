// [temp.dep.constexpr], [temp.inst], [dcl.enum]: the following enum sees the
// completed inferred enum for this specialization.
template<long long N>
struct MemberEnums {
  enum E { first = N };
  enum Size { bytes = sizeof(E) };
};

using FitsInt = MemberEnums<1073741824LL>;
using ExceedsUnsignedInt = MemberEnums<8589934592LL>;

static_assert(static_cast<unsigned char>(-1) == 255);
static_assert(sizeof(int) == 4 && sizeof(long) == 8 && sizeof(void*) == 8);
static_assert(FitsInt::bytes == sizeof(FitsInt::E));
static_assert(ExceedsUnsignedInt::bytes == sizeof(ExceedsUnsignedInt::E));
static_assert(FitsInt::bytes == sizeof(int));
static_assert(ExceedsUnsignedInt::bytes > sizeof(int));
