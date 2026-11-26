// The trait reads the effective specification of a completed move declaration.
struct Nonthrowing {
  Nonthrowing(Nonthrowing&&) noexcept(true);
};
struct PotentiallyThrowing {
  PotentiallyThrowing(PotentiallyThrowing&&) noexcept(false);
};

static_assert(__is_nothrow_constructible(Nonthrowing, Nonthrowing&&));
static_assert(!__is_nothrow_constructible(PotentiallyThrowing, PotentiallyThrowing&&));
