// [expr.log.and], [expr.log.or], [expr.cond], [class.temporary]
constexpr const int& hold(const int& value) { return value; }

static_assert(hold(42) == 42);
static_assert(true || hold(42) == 0);
static_assert(!(false && hold(42) == 0));
static_assert((true ? 42 : hold(0)) == 42);
static_assert((false ? hold(0) : 42) == 42);
static_assert((true ? hold(17) : hold(41)) == 17);
static_assert((false ? hold(17) : hold(41)) == 41);

constexpr bool within(bool left, bool right) {
  const int* saved = nullptr;
  return ((left && ((saved = &hold(17)), right)) || ((saved = &hold(41)), true)),
         *saved == (left && right ? 17 : 41);
}

static_assert(within(false, false));
static_assert(within(true, false));
static_assert(within(true, true));
