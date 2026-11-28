// [dcl.init.list]: the target of a direct list is the enum itself.
enum class Code : unsigned char { recorded = 19 };
enum Plain : unsigned char { idle = 0 };
constexpr Code direct{37};
constexpr Code copy = {Code::recorded};
constexpr Code empty = {};
constexpr Plain direct_plain{29};
static_assert(static_cast<unsigned>(direct) == 37);
static_assert(copy == Code::recorded);
static_assert(static_cast<unsigned>(empty) == 0);
static_assert(static_cast<unsigned>(direct_plain) == 29);

struct Slot { Code state; };
constexpr Slot slot{Code{41}};
constexpr Code entries[2] = {Code::recorded, {}};
static_assert(static_cast<unsigned>(slot.state) == 41);
static_assert(entries[0] == Code::recorded);
static_assert(static_cast<unsigned>(entries[1]) == 0);

constexpr Code echo(Code c) { return c; }
static_assert(echo(Code{37}) == direct);
