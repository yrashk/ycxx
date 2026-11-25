// #317, [expr.ref], [temp.arg.nontype]: project typed fields into member aliases.
struct Options {
  long long count;
  bool enabled;
};

template<long long> struct Count;
template<bool> struct Enabled;

template<Options V> struct Project {
  using count = Count<V.count>;
  using enabled = Enabled<V.enabled>;
};

using On = Project<Options{-4294967297LL, true}>;
using Off = Project<Options{4294967305LL, false}>;
static_assert(__is_same(On::count, Count<-4294967297LL>));
static_assert(__is_same(Off::count, Count<4294967305LL>));
static_assert(__is_same(On::enabled, Enabled<true>));
static_assert(__is_same(Off::enabled, Enabled<false>));
