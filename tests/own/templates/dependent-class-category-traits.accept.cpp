// Dependent predicates classify complete and incomplete enum and class types (#209).
template<bool, bool, bool> struct Kind {};

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

template<class T>
using Category = Kind<__is_enum(T), __is_union(T), __is_class(T)>;

template<class T> using Staged = Category<T>;

struct IncompleteStruct;
class IncompleteClass;
union IncompleteUnion;
enum NamedUnscoped { named_value };
enum class NamedScoped { value };
enum OpaqueUnscoped : unsigned;
enum class OpaqueScoped;
enum struct OpaqueScopedWithBase : unsigned char;

enum IncompleteEnum {
  classified_while_incomplete = Same<Staged<IncompleteEnum>, Kind<true, false, false>>::value
};

static_assert(classified_while_incomplete == 1);
static_assert(Same<Category<NamedUnscoped>, Kind<true, false, false>>::value);
static_assert(Same<Category<NamedScoped>, Kind<true, false, false>>::value);
static_assert(Same<Category<OpaqueUnscoped>, Kind<true, false, false>>::value);
static_assert(Same<Staged<const volatile OpaqueScoped>, Kind<true, false, false>>::value);
static_assert(Same<Category<OpaqueScopedWithBase>, Kind<true, false, false>>::value);

static_assert(Same<Category<IncompleteClass>, Kind<false, false, true>>::value);
static_assert(Same<Staged<const volatile IncompleteStruct>, Kind<false, false, true>>::value);
static_assert(Same<Staged<const volatile IncompleteUnion>, Kind<false, true, false>>::value);

static_assert(Same<Category<NamedUnscoped*>, Kind<false, false, false>>::value);
static_assert(Same<Category<const NamedScoped&>, Kind<false, false, false>>::value);
static_assert(Same<Category<IncompleteStruct*>, Kind<false, false, false>>::value);
static_assert(Same<Category<IncompleteClass&&>, Kind<false, false, false>>::value);
static_assert(Same<Category<IncompleteUnion* const>, Kind<false, false, false>>::value);
static_assert(Same<Category<volatile IncompleteUnion&>, Kind<false, false, false>>::value);

template<class T> struct ClassFamily {};
template<class T> class ClassFamily<T*> {};
template<> struct ClassFamily<int>;

template<class T> union UnionFamily {};
template<class T> union UnionFamily<T*> {};
template<> union UnionFamily<int>;

static_assert(Same<Staged<ClassFamily<long>>, Kind<false, false, true>>::value);
static_assert(Same<Staged<ClassFamily<long*>>, Kind<false, false, true>>::value);
static_assert(Same<Staged<ClassFamily<int>>, Kind<false, false, true>>::value);
static_assert(Same<Staged<UnionFamily<long>>, Kind<false, true, false>>::value);
static_assert(Same<Staged<UnionFamily<long*>>, Kind<false, true, false>>::value);
static_assert(Same<Staged<UnionFamily<int>>, Kind<false, true, false>>::value);

template<class T, bool IsEnum = __is_enum(T), bool IsUnion = __is_union(T),
         bool IsClass = __is_class(T)>
struct Choice {};

template<class T> using WithDefaults = Choice<T>;

static_assert(Same<WithDefaults<OpaqueUnscoped>, Choice<OpaqueUnscoped, true, false, false>>::value);
static_assert(Same<WithDefaults<IncompleteUnion>, Choice<IncompleteUnion, false, true, false>>::value);
static_assert(Same<WithDefaults<IncompleteStruct>, Choice<IncompleteStruct, false, false, true>>::value);
static_assert(Same<WithDefaults<NamedScoped*>, Choice<NamedScoped*, false, false, false>>::value);
static_assert(Same<Choice<IncompleteUnion, true>, Choice<IncompleteUnion, true, true, false>>::value);
static_assert(!Same<WithDefaults<IncompleteUnion>, Choice<IncompleteUnion, true, true, false>>::value);
