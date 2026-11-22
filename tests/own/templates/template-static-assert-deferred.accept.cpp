// #241: a false assertion has no effect in an uninstantiated template
// definition or an instantiated discarded statement ([dcl.pre], [temp.res.general]).
template<class T> void unused_function() {
  static_assert(false);
}

template<class T> struct UnusedClass {
  void unused_member() { static_assert(false, "member must be instantiated first"); }
};

UnusedClass<int> object;

template<class T> constexpr int checked() {
  if constexpr (sizeof(T) == sizeof(char)) {
    return 7;
  } else {
    static_assert(false, "char-sized type required");
  }
}

static_assert(checked<char>() == 7);
