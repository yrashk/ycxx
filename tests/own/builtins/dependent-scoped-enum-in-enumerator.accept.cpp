// #1029 [dcl.enum], [meta.unary.prop], [temp.arg.nontype].
template<bool Value>
struct ScopedFlag {
  static constexpr bool value = Value;
};

template<class T>
constexpr int scoped_value() {
  return ScopedFlag<__is_scoped_enum(T)>::value;
}

enum class Stage {
  from_trait = scoped_value<Stage>()
};

static_assert(static_cast<int>(Stage::from_trait) == 1);
