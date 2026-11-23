// #1042 [class.pre], [meta.unary.prop], [temp.arg.nontype].
template<bool Value>
struct FinalFlag {
  static constexpr bool value = Value;
};

template<class T>
struct FinalOf : FinalFlag<__is_final(T)> {};

template<class T>
inline constexpr bool final_v = __is_final(T);

struct Closed final {};
struct Open {};
union Storage { int value; };

static_assert(FinalOf<Closed>::value);
static_assert(!final_v<Open>);
static_assert(FinalOf<const volatile Closed>::value && final_v<const volatile Closed>);
static_assert(!FinalOf<const Open>::value && !final_v<const Open>);
static_assert(!FinalOf<int>::value && !final_v<int>);
static_assert(!FinalOf<Closed&>::value && !final_v<Closed&>);
static_assert(!FinalOf<Storage>::value && !final_v<Storage>);
static_assert(!FinalOf<Open>::value);
static_assert(final_v<Closed>);

struct Later;
FinalOf<Later>* deferred;
struct Later final {};
static_assert(FinalOf<Later>::value && final_v<Later>);
