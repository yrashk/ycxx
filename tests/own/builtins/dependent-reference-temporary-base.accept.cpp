// #1064 [meta.unary.prop], [dcl.init.ref], [temp.arg.nontype].
template<bool Direct, bool Copy>
struct Result {
  static constexpr bool direct = Direct;
  static constexpr bool copy = Copy;
};

template<class Target, class Source>
struct Probe : Result<__reference_constructs_from_temporary(Target, Source),
                      __reference_converts_from_temporary(Target, Source)> {};

template<class Source>
struct IntoInt : Result<__reference_constructs_from_temporary(const int&, Source),
                        __reference_converts_from_temporary(const int&, Source)> {};

template<class Target>
struct FromInt : Result<__reference_constructs_from_temporary(Target, int),
                        __reference_converts_from_temporary(Target, int)> {};

static_assert(Probe<const double&, int&>::direct && Probe<const double&, int&>::copy);
static_assert(IntoInt<short&>::direct && IntoInt<short&>::copy);
static_assert(!IntoInt<int&&>::direct && !IntoInt<int&&>::copy);
static_assert(FromInt<int&&>::direct && FromInt<int&&>::copy);
static_assert(!FromInt<int&>::direct && !FromInt<int&>::copy);
static_assert(!FromInt<long>::direct && !FromInt<long>::copy);

struct Later;
Probe<const Later&, Later>* deferred;
struct Later { int value; };

static_assert(Probe<const Later&, Later>::direct && Probe<const Later&, Later>::copy);
static_assert(!Probe<const Later&, Later&>::direct && !Probe<const Later&, Later&>::copy);
