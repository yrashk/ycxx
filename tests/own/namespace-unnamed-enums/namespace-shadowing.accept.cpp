enum { offset = 3 };

namespace range {
constexpr int outer_value = offset;
enum { offset = ::offset + 5, following };

static_assert(outer_value == 3);
static_assert(offset == 8);
static_assert(following == 9);
}

static_assert(offset == 3);
static_assert(range::offset == 8);
static_assert(range::following == 9);
