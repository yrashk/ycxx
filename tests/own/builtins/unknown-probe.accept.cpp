// #229: a `__builtin_` name that is neither a built-in ycxx has nor one it knows it lacks
// (docs/EXTENSIONS.md) is an ordinary name: undeclared, so a requires-expression probing it is
// unsatisfied, not ill-formed ([expr.prim.req], [temp.constr.atomic]), as libycxx's in-language
// probes need.
template <class T>
concept Probe = requires(T t) { __builtin_no_such_thing(t); };
static_assert(!Probe<int>);

template <class T>
constexpr int pick() {
  if constexpr (requires(T t) { __builtin_no_such_thing(t); })
    return 1;
  else
    return 2;
}
static_assert(pick<int>() == 2);
