// #209 [temp.dep.constexpr], [temp.arg.nontype], [temp.alias], [class.derived.general].
template<int Code>
struct Category {};

template<class T>
consteval int category_code() { return sizeof(T) == 2 ? 17 : 29; }

template<class T>
using CategoryFor = Category<category_code<T>()>;

template<class T>
struct Record : Category<category_code<T>()> {};

static_assert(__is_same(Category<category_code<char[2]>()>, Category<17>));
static_assert(__is_same(CategoryFor<char[2]>, Category<17>));
static_assert(__is_same(CategoryFor<char[5]>, Category<29>));
static_assert(__is_base_of(Category<17>, Record<char[2]>));
static_assert(__is_base_of(Category<29>, Record<char[5]>));
