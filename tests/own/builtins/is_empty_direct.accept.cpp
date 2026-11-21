struct Empty {};
struct StaticOnly {
    static int value;
};
struct Nonempty {
    int value;
};
struct PrivateEmpty : private Empty {};
struct DirectNonempty : Nonempty {};
struct TransitiveNonempty : private DirectNonempty {};

union EmptyUnion {};
union ForwardUnion;

static_assert(__is_empty(Empty));
static_assert(__is_empty(StaticOnly));
static_assert(__is_empty(PrivateEmpty));
static_assert(!__is_empty(Nonempty));
static_assert(!__is_empty(DirectNonempty));
static_assert(!__is_empty(TransitiveNonempty));

static_assert(!__is_empty(EmptyUnion));
static_assert(!__is_empty(ForwardUnion));
static_assert(!__is_empty(int));
static_assert(!__is_empty(Empty*));
static_assert(!__is_empty(Empty&));
