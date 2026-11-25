struct Incomplete;
enum Color { red };
enum class Mode { idle };
enum class Opaque : unsigned;

using Function = void(int);
using DataMember = int Incomplete::*;
using MemberFunction = void (Incomplete::*)(int);
using Matrix = const int[2][3];
using UnknownMatrix = int[][3];

static_assert(__is_trivial(int) && __is_pod(int));
static_assert(__is_trivial(double) && __is_pod(double));
static_assert(__is_trivial(Color) && __is_pod(Color));
static_assert(__is_trivial(const volatile Mode) && __is_pod(const volatile Mode));
static_assert(__is_trivial(Opaque) && __is_pod(Opaque));
static_assert(__is_trivial(decltype(nullptr)) && __is_pod(decltype(nullptr)));
static_assert(__is_trivial(Incomplete* const) && __is_pod(Incomplete* const));
static_assert(__is_trivial(Function*) && __is_pod(Function*));
static_assert(__is_trivial(DataMember) && __is_pod(DataMember));
static_assert(__is_trivial(MemberFunction) && __is_pod(MemberFunction));

static_assert(__is_trivial(Matrix) && __is_pod(Matrix));
static_assert(__is_trivial(UnknownMatrix) && __is_pod(UnknownMatrix));
static_assert(__is_trivial(Incomplete*[]) && __is_pod(Incomplete*[]));

static_assert(!__is_trivial(Incomplete&) && !__is_pod(Incomplete&));
static_assert(!__is_trivial(Incomplete&&) && !__is_pod(Incomplete&&));
static_assert(!__is_trivial(Function&) && !__is_pod(Function&));
static_assert(!__is_trivial(Matrix&) && !__is_pod(Matrix&));
static_assert(!__is_trivial(Function) && !__is_pod(Function));
static_assert(!__is_trivial(const volatile void) && !__is_pod(const volatile void));

template<class> constexpr bool is_bool = false;
template<> constexpr bool is_bool<bool> = true;
static_assert(is_bool<decltype(__is_trivial(int))>);
static_assert(is_bool<decltype(__is_pod(int))>);

template<bool Value> struct Flag { static constexpr bool value = Value; };
template<class T> struct Trivial : Flag<__is_trivial(T)> {};
template<class T> struct Pod : Flag<__is_pod(T)> {};

template<class T, bool Expected>
constexpr bool matches = Trivial<T>::value == Expected && Pod<T>::value == Expected;

static_assert(matches<int, true> && matches<double, true>);
static_assert(matches<Color, true> && matches<const volatile Mode, true> && matches<Opaque, true>);
static_assert(matches<decltype(nullptr), true> && matches<Incomplete* const, true>);
static_assert(matches<Function*, true> && matches<DataMember, true> && matches<MemberFunction, true>);
static_assert(matches<Matrix, true> && matches<UnknownMatrix, true> && matches<Incomplete*[], true>);
static_assert(matches<Incomplete&, false> && matches<Incomplete&&, false>);
static_assert(matches<Function&, false> && matches<Matrix&, false>);
static_assert(matches<Function, false> && matches<void, false> && matches<const volatile void, false>);

template<int Bound>
struct Bounded : Flag<__is_trivial(const int[Bound][3]) && __is_pod(const int[Bound][3])> {};

static_assert(Bounded<2>::value);
