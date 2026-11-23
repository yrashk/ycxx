struct Limits {
  int requested;
  int allowed = requested;
};

struct Low {
  static constexpr int requested = 2;
};

struct High {
  static constexpr int requested = 5;
};

template<class T>
struct Profile {
  static constexpr int fixed = Limits{2}.allowed;
  static constexpr int uses = Limits{T::requested}.allowed;
};

static_assert(Profile<Low>::fixed == 2);
static_assert(Profile<Low>::uses == 2);
static_assert(Profile<High>::uses == 5);

constexpr Limits named{3};
static_assert(named.allowed == 3);
static_assert(Limits{4, 8}.allowed == 8);
