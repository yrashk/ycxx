// #218: An ordinary function type is outside the trivially copyable type categories.
using Function = int(double);
static_assert(!__is_trivially_copyable(Function));
