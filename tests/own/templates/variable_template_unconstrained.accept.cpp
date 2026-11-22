template<class T>
constexpr int token = 43;

static_assert(__is_same(decltype(token<unsigned int>), const int));
constexpr const int* address = &token<unsigned int>;
static_assert(*address == 43);

template<class T>
constexpr int read = token<T>;

template<class T>
constexpr const int* read_address = &token<T>;

static_assert(read<unsigned int> == 43);
static_assert(*read_address<char> == 43);
