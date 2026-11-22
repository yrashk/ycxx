enum class Mode : unsigned char { audit = 3, enforce = 9 };
enum Result : signed char { denied = -7, granted = 5 };

struct PolicyDescriptor {
  Mode mode;
  mutable Result result;
};

constexpr PolicyDescriptor policy{Mode::enforce, denied};
static_assert(policy.mode == Mode::enforce);
static_assert(policy.result == denied); //! error
