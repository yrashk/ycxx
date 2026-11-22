template<class From, class To>
constexpr bool convertible = __is_convertible(From, To);

struct BaseTag {};
struct FirstTag : public BaseTag {};
struct SecondTag : public FirstTag {};
struct DerivedTag : public SecondTag {};

static_assert(convertible<DerivedTag, BaseTag>);
static_assert(convertible<const DerivedTag&, BaseTag>);
static_assert(convertible<const DerivedTag&, const BaseTag&>);
static_assert(convertible<DerivedTag&&, BaseTag&&>);
