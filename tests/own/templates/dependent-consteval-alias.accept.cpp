// #209 [temp.dep.constexpr], [temp.arg.nontype], [temp.alias].
consteval int bucket_index(int value) { return value * 2 + 1; }

template<int Index>
struct Bucket {};

template<int Value>
using BucketFor = Bucket<bucket_index(Value)>;

static_assert(__is_same(Bucket<bucket_index(3)>, Bucket<7>));
static_assert(__is_same(BucketFor<2>, Bucket<5>));
static_assert(__is_same(BucketFor<6>, Bucket<13>));
