// [expr.const], [conv.ptr], [expr.eq]
constexpr int scalar = 42;
int values[] = {19, 23, 42};
constexpr const void* erased_scalar = &scalar;
constexpr const void* erased_whole = &values;
constexpr const void* erased_first = values;
constexpr const void* erased_inside = values + 1;
constexpr const void* erased_past = values + 3;
constexpr const volatile void* erased_cv = values + 2;
constexpr const volatile void* erased_null = nullptr;

static_assert(erased_scalar == static_cast<const void*>(&scalar));
static_assert(erased_whole == static_cast<const void*>(&values));
static_assert(erased_first == static_cast<const void*>(values));
static_assert(erased_inside == static_cast<const void*>(values + 1));
static_assert(erased_past == static_cast<const void*>(values + 3));
static_assert(erased_past != erased_inside);
static_assert(erased_cv == static_cast<const volatile void*>(values + 2));
static_assert(erased_null == nullptr);
