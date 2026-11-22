enum { first_tag = 1, first_next };
enum { second_tag = 1, second_next };

static_assert(__is_same(decltype(first_tag), decltype(first_next)));
static_assert(__is_same(decltype(second_tag), decltype(second_next)));
static_assert(!__is_same(decltype(first_tag), decltype(second_tag)));
static_assert(!__is_same(decltype(first_tag), int));
static_assert(__is_same(decltype(+first_tag), int));
static_assert(+first_tag == +second_tag);
static_assert(+first_next == +second_next);
