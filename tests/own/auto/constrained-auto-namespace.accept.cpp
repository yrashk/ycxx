// Support a type-constrained placeholder on an ordinary namespace variable.
template<class T> concept IsInt = __is_same(T, int);
IsInt auto count = 17;
static_assert(__is_same(decltype(count), int));
