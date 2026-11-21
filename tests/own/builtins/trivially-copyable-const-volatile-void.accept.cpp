// #218: Cv-qualified void is outside the trivially copyable type categories.
static_assert(!__is_trivially_copyable(const volatile void));
