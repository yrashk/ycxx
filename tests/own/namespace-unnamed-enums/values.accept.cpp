enum {
  default_zero,
  default_one,
  explicit_negative = -3,
  after_negative,
  from_prior = default_one - after_negative,
  after_prior,
};

static_assert(default_zero == 0);
static_assert(default_one == 1);
static_assert(explicit_negative == -3);
static_assert(after_negative == -2);
static_assert(from_prior == 3);
static_assert(after_prior == 4);

constexpr int scalar = after_prior;
static_assert(scalar == 4);
