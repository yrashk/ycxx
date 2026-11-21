// Dependent member-pointer predicates select literal bool specializations (#209).
template<bool, bool, bool> struct Category {};

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

template<class T>
using Traits = Category<__is_member_pointer(T), __is_member_object_pointer(T), __is_member_function_pointer(T)>;

template<class T> using Staged = Traits<T>;
template<class T> using ConstVolatile = T const volatile;

struct Owner {
  int data;
};
struct Incomplete;

using ObjectMember = int Owner::*;
using ConstObjectMember = const int Owner::*;
using LvalueFunctionMember = int (Owner::*)(short) const volatile & noexcept;
using RvalueFunctionMember = int (Owner::*)(short) &&;
using IncompleteObjectMember = int Incomplete::*;
using IncompleteFunctionMember = int (Incomplete::*)(short) const &;
using Function = int(short) noexcept;
using FunctionPointer = int (*)(short) noexcept;

using Object = Category<true, true, false>;
using MemberFunction = Category<true, false, true>;
using Neither = Category<false, false, false>;

static_assert(Same<Traits<ObjectMember>, Object>::value);
static_assert(Same<Traits<ConstObjectMember>, Object>::value);
static_assert(Same<Staged<ConstVolatile<ObjectMember>>, Object>::value);
static_assert(Same<Traits<LvalueFunctionMember>, MemberFunction>::value);
static_assert(Same<Staged<ConstVolatile<RvalueFunctionMember>>, MemberFunction>::value);
static_assert(Same<Staged<IncompleteObjectMember>, Object>::value);
static_assert(Same<Staged<IncompleteFunctionMember>, MemberFunction>::value);

static_assert(Same<Traits<int*>, Neither>::value);
static_assert(Same<Traits<Function>, Neither>::value);
static_assert(Same<Traits<FunctionPointer>, Neither>::value);
static_assert(Same<Traits<FunctionPointer const>, Neither>::value);
static_assert(Same<Traits<ObjectMember&>, Neither>::value);
static_assert(Same<Traits<LvalueFunctionMember&&>, Neither>::value);
static_assert(Same<Traits<Owner&>, Neither>::value);

template<class T, bool Any = __is_member_pointer(T), bool Data = __is_member_object_pointer(T),
         bool Callable = __is_member_function_pointer(T)>
struct Defaults {};

template<class T> using StagedDefaults = Defaults<T>;

static_assert(Same<StagedDefaults<ObjectMember>, Defaults<ObjectMember, true, true, false>>::value);
static_assert(Same<StagedDefaults<LvalueFunctionMember>, Defaults<LvalueFunctionMember, true, false, true>>::value);
static_assert(Same<StagedDefaults<RvalueFunctionMember&&>, Defaults<RvalueFunctionMember&&, false, false, false>>::value);
static_assert(Same<Defaults<RvalueFunctionMember, false, true>, Defaults<RvalueFunctionMember, false, true, true>>::value);
static_assert(!Same<StagedDefaults<ObjectMember>, Defaults<ObjectMember, true, false, true>>::value);
