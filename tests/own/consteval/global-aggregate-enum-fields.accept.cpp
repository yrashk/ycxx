enum class Mode : unsigned char { audit = 3, enforce = 9 };
enum Result : signed char { denied = -7, granted = 5 };

struct PolicyDescriptor {
  Mode mode;
  int uses;
  Result result;
};

constexpr PolicyDescriptor policy{Mode::enforce, 14, denied};

static_assert(policy.mode == Mode::enforce);
static_assert(policy.uses == 14);
static_assert(policy.result == denied);
static_assert(static_cast<int>(policy.result) == -7);

template<Mode Selected>
struct Selection {
  static constexpr Mode value = Selected;
};

template<Result Selected>
struct Decision {
  static constexpr int value = Selected;
};

static_assert(Selection<policy.mode>::value == Mode::enforce);
static_assert(Decision<policy.result>::value == -7);
