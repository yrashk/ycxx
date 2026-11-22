// [basic.scope.class], [basic.scope.enum]: enum names take precedence over the enclosing class.
struct Box {
  static constexpr int offset = 7;
  enum Unscoped { first = offset, second = first + 1 };
  using Underlying = int;
  enum class Scoped : Underlying { earlier = offset, offset = 11, local = offset, qualified = Box::offset,
                                   injected = second };
};

static_assert(Box::first == 7 && Box::second == 8);
static_assert(static_cast<int>(Box::Scoped::earlier) == 7);
static_assert(static_cast<int>(Box::Scoped::local) == 11);
static_assert(static_cast<int>(Box::Scoped::qualified) == 7);
static_assert(static_cast<int>(Box::Scoped::injected) == 8);

template<class T> struct Generic {
  static constexpr int offset = 3;
  enum class Scoped { first = offset, offset = 5, second = offset };
};

static_assert(static_cast<int>(Generic<int>::Scoped::first) == 3);
static_assert(static_cast<int>(Generic<int>::Scoped::second) == 5);
