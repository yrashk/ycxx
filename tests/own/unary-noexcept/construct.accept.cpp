// [expr.unary.noexcept], [except.spec]
struct Owner { Owner(Owner&&) noexcept {} };
struct RiskyOwner { RiskyOwner(RiskyOwner&&) {} };

void check_moves(Owner&& owner, RiskyOwner&& risky) {
  static_assert(noexcept(Owner(static_cast<Owner&&>(owner))));
  static_assert(!noexcept(RiskyOwner(static_cast<RiskyOwner&&>(risky))));
}

struct Selected {
  Selected(int) noexcept;
  Selected(double);
};

static_assert(noexcept(Selected(1)));
static_assert(!noexcept(Selected(1.0)));
