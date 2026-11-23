// #157 [class.mem.general], [over.match.viable], [over.ics.rank], [except.spec].
// The default argument makes the int overload viable and the destructor potentially throwing.
struct DestructionQuery {
  static long pick(int, int = 0);
  static char pick(long);

  ~DestructionQuery() noexcept(sizeof(pick(0)) != sizeof(long)) = default;

  static constexpr bool query(bool value = noexcept(((DestructionQuery*)nullptr)->~DestructionQuery())) {
    return value;
  }
};

static_assert(!DestructionQuery::query());
static_assert(!noexcept(((DestructionQuery*)nullptr)->~DestructionQuery()));
