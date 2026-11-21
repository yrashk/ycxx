// #218: An rvalue reference is outside the trivially copyable type categories.
static_assert(!__is_trivially_copyable(int&&));
