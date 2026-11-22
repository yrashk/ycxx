namespace probe {
template<class T>
requires (sizeof(T) > 1)
constexpr int token = 31;
}

static_assert(__is_same(decltype(probe::token<int>), const int));
constexpr const int* address = &probe::token<long>;
static_assert(*address == 31);
