// #1029 [meta.unary.prop], [dcl.enum], [temp.arg.nontype].
template<bool Value>
struct ScopedFlag {
  static constexpr bool value = Value;
};

template<class T>
struct Scoped : ScopedFlag<__is_scoped_enum(T)> {};

enum class Mode : unsigned { ready };
enum LegacyMode : unsigned { legacy_ready };

static_assert(Scoped<Mode>::value);
static_assert(!Scoped<LegacyMode>::value);
static_assert(!Scoped<unsigned>::value);
static_assert(Scoped<const volatile Mode>::value);
static_assert(!Scoped<const LegacyMode>::value);
static_assert(!Scoped<Mode&>::value);
static_assert(Scoped<Mode>::value);
