struct Root {};
struct Private : private Root {};
struct DeepPrivate : private Private {};
struct Left : Root {};
struct Right : Root {};
struct Ambiguous : Left, Right {};

struct Forward;
struct Unrelated;
union Union {
    int value;
};
union ForwardUnion;

static_assert(__is_base_of(const Root, volatile Root));
static_assert(__is_base_of(const Forward, volatile Forward));
static_assert(__is_base_of(Root, Private));
static_assert(__is_base_of(const Root, const volatile DeepPrivate));
static_assert(__is_base_of(Root, Ambiguous));

static_assert(!__is_base_of(Ambiguous, Root));
static_assert(!__is_base_of(Unrelated, Ambiguous));
static_assert(!__is_base_of(Union, Union));
static_assert(!__is_base_of(const ForwardUnion, volatile ForwardUnion));
static_assert(!__is_base_of(Root*, Root*));
static_assert(!__is_base_of(Root&, Root&));
static_assert(!__is_base_of(Root*, Private));
static_assert(!__is_base_of(int, int));
