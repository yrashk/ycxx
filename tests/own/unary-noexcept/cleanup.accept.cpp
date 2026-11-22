// [expr.unary.noexcept], [conv.rval], [except.spec]
struct ThrowingCleanup {
  ThrowingCleanup() noexcept;
  ~ThrowingCleanup() noexcept(false);
};

struct SafeCleanup {
  SafeCleanup() noexcept;
  ~SafeCleanup() noexcept;
};

ThrowingCleanup make_throwing_cleanup() noexcept;
SafeCleanup make_safe_cleanup() noexcept;
int use(const ThrowingCleanup&) noexcept;
int use(const SafeCleanup&) noexcept;

static_assert(!noexcept(ThrowingCleanup()));
static_assert(!noexcept(make_throwing_cleanup()));
static_assert(!noexcept(use(make_throwing_cleanup())));
static_assert(noexcept(SafeCleanup()));
static_assert(noexcept(use(make_safe_cleanup())));
