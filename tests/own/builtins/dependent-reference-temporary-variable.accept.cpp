// #1064 [meta.unary.prop], [dcl.init.ref], [temp.arg.nontype].
template<bool Value> inline constexpr bool flag_v = Value;

template<class Target, class Source>
inline constexpr bool constructs_v = __reference_constructs_from_temporary(Target, Source);
template<class Target, class Source>
inline constexpr bool converts_v = __reference_converts_from_temporary(Target, Source);

template<class Target, class Source>
inline constexpr bool constructs_arg_v =
    flag_v<__reference_constructs_from_temporary(Target, Source)>;
template<class Target, class Source>
inline constexpr bool converts_arg_v =
    flag_v<__reference_converts_from_temporary(Target, Source)>;

struct Later;
static_assert(constructs_v<const double&, int&> && converts_v<const double&, int&>);
static_assert(!constructs_v<const int&, int&> && !converts_v<const int&, int&>);
static_assert(constructs_arg_v<const int&, int> && converts_arg_v<const int&, int>);
static_assert(!constructs_arg_v<int&, int> && !converts_arg_v<int&, int>);

static_assert(__reference_constructs_from_temporary(const double&, int&));
static_assert(__reference_converts_from_temporary(int&&, int));
static_assert(!__reference_constructs_from_temporary(int&, int));
static_assert(!__reference_converts_from_temporary(const int&, int&&));

struct Later { int value; };
static_assert(constructs_v<const Later&, Later> && converts_v<const Later&, Later>);
static_assert(!constructs_arg_v<const Later&, Later&> && !converts_arg_v<const Later&, Later&>);
