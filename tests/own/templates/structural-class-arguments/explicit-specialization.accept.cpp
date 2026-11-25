// [temp.expl.spec], [temp.type]: an explicit specialization is selected by structural value.
struct Key { int value; };
constexpr Key one{1};

template<Key> struct Pick { using type = int; };
template<> struct Pick<Key{1}> { using type = long; };

static_assert(__is_same(Pick<one>::type, long));
static_assert(__is_same(Pick<Key{1}>::type, long));
static_assert(__is_same(Pick<Key{2}>::type, int));
