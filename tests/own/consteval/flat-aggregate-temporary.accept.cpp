// [class.temporary], [expr.const.core]: accesses are within the temporary's lifetime.
enum class Capability : unsigned short { none, write = 41 };
enum Mode : signed char { deny = -3, unset = 0, grant = 2 };

struct Limits {
  unsigned short quota;
  Capability capability;
  Mode mode;
  bool active;
  unsigned long long bytes;
};

constexpr Limits stored{17, Capability::write, deny, true, 0x100000003ULL};
static_assert(stored.quota == 17);
static_assert(stored.capability == Capability::write);

static_assert(Limits{42, Capability::write, deny, true, 0x100000005ULL}.quota == 42);
static_assert(Limits{42, Capability::write, deny, true, 0x100000005ULL}.capability == Capability::write);
static_assert(Limits{42, Capability::write, deny, true, 0x100000005ULL}.mode == deny);
static_assert(Limits{42, Capability::write, deny, true, 0x100000005ULL}.active);
static_assert(Limits{42, Capability::write, deny, true, 0x100000005ULL}.bytes == 0x100000005ULL);
static_assert(Limits{}.mode == unset);
static_assert(Limits().bytes == 0);
