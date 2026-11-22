// Dependent array, pointer, and function predicates select literal bool specializations (#209).
template<bool, bool, bool> struct Shape {};

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

template<class T>
using Traits = Shape<__is_array(T), __is_pointer(T), __is_function(T)>;

template<class T> using Staged = Traits<T>;
template<class T> using AddPointer = T*;
template<class T> using AddReference = T&;

using BoundedArray = int[3];
using UnboundedArray = int[];
using QualifiedArray = const volatile int[3];
using QualifiedUnboundedArray = const int[];
using MultidimensionalArray = int[2][3];
using ArrayOfPointers = int*[3];
using Function = long(short);
using NoThrowFunction = long(short) noexcept;
using QualifiedFunctionPointer = long (*const volatile)(short);

struct Record;
using DataMemberPointer = int Record::*;
using MemberFunctionPointer = long (Record::*)(short);

static_assert(Same<Traits<BoundedArray>, Shape<true, false, false>>::value);
static_assert(Same<Traits<UnboundedArray>, Shape<true, false, false>>::value);
static_assert(Same<Traits<QualifiedArray>, Shape<true, false, false>>::value);
static_assert(Same<Traits<QualifiedUnboundedArray>, Shape<true, false, false>>::value);
static_assert(Same<Staged<MultidimensionalArray>, Shape<true, false, false>>::value);
static_assert(Same<Traits<ArrayOfPointers>, Shape<true, false, false>>::value);
static_assert(Same<Staged<AddReference<BoundedArray>>, Shape<false, false, false>>::value);

static_assert(Same<Traits<int*>, Shape<false, true, false>>::value);
static_assert(Same<Traits<const int*>, Shape<false, true, false>>::value);
static_assert(Same<Traits<int* const volatile>, Shape<false, true, false>>::value);
static_assert(Same<Traits<int*&>, Shape<false, false, false>>::value);
static_assert(Same<Staged<AddPointer<BoundedArray>>, Shape<false, true, false>>::value);

static_assert(Same<Traits<Function>, Shape<false, false, true>>::value);
static_assert(Same<Staged<NoThrowFunction>, Shape<false, false, true>>::value);
static_assert(Same<Staged<AddPointer<NoThrowFunction>>, Shape<false, true, false>>::value);
static_assert(Same<Traits<QualifiedFunctionPointer>, Shape<false, true, false>>::value);
static_assert(Same<Staged<AddReference<Function>>, Shape<false, false, false>>::value);
static_assert(Same<Traits<Function&&>, Shape<false, false, false>>::value);

static_assert(Same<Traits<DataMemberPointer>, Shape<false, false, false>>::value);
static_assert(Same<Traits<MemberFunctionPointer>, Shape<false, false, false>>::value);
static_assert(Same<Traits<void>, Shape<false, false, false>>::value);

template<class T, bool IsArray = __is_array(T), bool IsPointer = __is_pointer(T),
         bool IsFunction = __is_function(T)>
struct Choice {};

template<class T> using WithDefaults = Choice<T>;

static_assert(Same<WithDefaults<UnboundedArray>, Choice<UnboundedArray, true, false, false>>::value);
static_assert(Same<WithDefaults<int* const>, Choice<int* const, false, true, false>>::value);
static_assert(Same<WithDefaults<Function>, Choice<Function, false, false, true>>::value);
static_assert(Same<WithDefaults<DataMemberPointer>,
                   Choice<DataMemberPointer, false, false, false>>::value);
static_assert(Same<Choice<Function, false, true>, Choice<Function, false, true, true>>::value);
static_assert(!Same<WithDefaults<Function>, Choice<Function, false, true, true>>::value);
