// Dependent object classification selects the matching bool specialization and default (#209).
template<bool Value> struct Flag {
    static constexpr bool value = Value;
};

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

template<class T> using ObjectFlag = Flag<__is_object(T)>;
template<class T> using StagedFlag = ObjectFlag<T>;

template<class T, bool Value = __is_object(T)> struct WithDefault {
    static constexpr bool value = Value;
};

struct Complete { int member; };
struct Pending;
union CompleteUnion { int member; };
union PendingUnion;
enum class Enumeration { value };

using Array = int[3];
using UnknownBound = const int[];
using Matrix = volatile int[2][3];
using Function = long(short) noexcept;
using FunctionPointer = Function*;
using DataMemberPointer = int Pending::*;
using FunctionMemberPointer = long (Complete::*)(short) const & noexcept;

static_assert(StagedFlag<Complete>::value);
static_assert(StagedFlag<Pending>::value);
static_assert(StagedFlag<CompleteUnion>::value);
static_assert(StagedFlag<const volatile PendingUnion>::value);
static_assert(StagedFlag<Enumeration>::value);
static_assert(StagedFlag<int>::value);
static_assert(StagedFlag<decltype(nullptr)>::value);
static_assert(StagedFlag<Array>::value);
static_assert(StagedFlag<UnknownBound>::value);
static_assert(StagedFlag<Matrix>::value);
static_assert(StagedFlag<void*>::value);
static_assert(StagedFlag<FunctionPointer>::value);
static_assert(StagedFlag<DataMemberPointer>::value);
static_assert(StagedFlag<FunctionMemberPointer>::value);

static_assert(!StagedFlag<void>::value);
static_assert(!StagedFlag<const volatile void>::value);
static_assert(!StagedFlag<int&>::value);
static_assert(!StagedFlag<Pending&&>::value);
static_assert(!StagedFlag<Array&>::value);
static_assert(!StagedFlag<Function>::value);
static_assert(!StagedFlag<Function&>::value);

static_assert(Same<StagedFlag<UnknownBound>, Flag<true>>::value);
static_assert(Same<StagedFlag<Function>, Flag<false>>::value);
static_assert(Same<WithDefault<Pending>, WithDefault<Pending, true>>::value);
static_assert(Same<WithDefault<const void>, WithDefault<const void, false>>::value);
static_assert(WithDefault<FunctionPointer>::value);
static_assert(!WithDefault<Function>::value);
static_assert(!WithDefault<Complete, false>::value);
static_assert(WithDefault<void, true>::value);
