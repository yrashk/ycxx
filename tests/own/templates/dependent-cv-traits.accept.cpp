// Dependent cv predicates preserve top-level qualification and bool identity (#209).
template<bool, bool> struct Flags;

template<> struct Flags<false, false> { static constexpr int value = 0; };
template<> struct Flags<true, false> { static constexpr int value = 1; };
template<> struct Flags<false, true> { static constexpr int value = 2; };
template<> struct Flags<true, true> { static constexpr int value = 3; };

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

template<class T> using Qualifiers = Flags<__is_const(T), __is_volatile(T)>;
template<class T> using Staged = Qualifiers<T>;
template<class T> using AddCv = const volatile T;

static_assert(Same<Qualifiers<int>, Flags<false, false>>::value);
static_assert(Same<Qualifiers<const int>, Flags<true, false>>::value);
static_assert(Same<Qualifiers<volatile int>, Flags<false, true>>::value);
static_assert(Same<Qualifiers<const volatile int>, Flags<true, true>>::value);
static_assert(Same<Staged<AddCv<void>>, Flags<true, true>>::value);

static_assert(Same<Staged<const int&>, Flags<false, false>>::value);
static_assert(Same<Staged<volatile int&>, Flags<false, false>>::value);
static_assert(Same<Staged<const int&&>, Flags<false, false>>::value);
static_assert(Same<Staged<volatile int&&>, Flags<false, false>>::value);
static_assert(Same<Staged<const volatile int&&>, Flags<false, false>>::value);
static_assert(Same<Staged<AddCv<int&>>, Flags<false, false>>::value);
static_assert(Same<Staged<AddCv<int&&>>, Flags<false, false>>::value);

static_assert(Same<Qualifiers<const volatile int*>, Flags<false, false>>::value);
static_assert(Same<Qualifiers<int* const>, Flags<true, false>>::value);
static_assert(Same<Qualifiers<const int* volatile>, Flags<false, true>>::value);
static_assert(Same<Qualifiers<volatile int* const>, Flags<true, false>>::value);
static_assert(Same<Qualifiers<int* const volatile>, Flags<true, true>>::value);

using Array = int[3];
using ConstArray = const int[3];
using VolatileArray = volatile int[3];
using IncompleteArray = int[];
using ArrayOfConstPointers = int* const[3];
using ArrayOfPointersToConst = const int*[3];

static_assert(Same<Qualifiers<Array>, Flags<false, false>>::value);
static_assert(Same<Qualifiers<ConstArray>, Flags<true, false>>::value);
static_assert(Same<Qualifiers<VolatileArray>, Flags<false, true>>::value);
static_assert(Same<Staged<AddCv<IncompleteArray>>, Flags<true, true>>::value);
static_assert(Same<Staged<AddCv<Array>>, Flags<true, true>>::value);
static_assert(Same<Qualifiers<ConstArray&>, Flags<false, false>>::value);
static_assert(Same<Qualifiers<ArrayOfConstPointers>, Flags<true, false>>::value);
static_assert(Same<Qualifiers<ArrayOfPointersToConst>, Flags<false, false>>::value);

template<class T, bool IsConst = __is_const(T), bool IsVolatile = __is_volatile(T)>
struct Choice : Flags<IsConst, IsVolatile> {};

template<class T> using WithDefaults = Choice<T>;

static_assert(Same<WithDefaults<const volatile int>, Choice<const volatile int, true, true>>::value);
static_assert(Same<WithDefaults<volatile int>, Choice<volatile int, false, true>>::value);
static_assert(Same<WithDefaults<const int&>, Choice<const int&, false, false>>::value);
static_assert(Same<WithDefaults<VolatileArray>, Choice<VolatileArray, false, true>>::value);
static_assert(Same<Choice<const volatile int, false>, Choice<const volatile int, false, true>>::value);
static_assert(!Same<WithDefaults<const volatile int>, Choice<const volatile int, false, true>>::value);
static_assert(Choice<const volatile int, false>::value == 2);
static_assert(Choice<const volatile int, true, false>::value == 1);
static_assert(Choice<int, true, true>::value == 3);
