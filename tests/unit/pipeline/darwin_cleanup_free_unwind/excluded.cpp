extern "C" int excluded_noexcept(int value) noexcept {
  return value + 8;
}

template<int Extra>
inline int excluded_specialization(int value) {
  return value + Extra;
}

extern "C" int use_specialization(int value) {
  return excluded_specialization<9>(value);
}

extern "C" void observe_drop(int) noexcept;

struct Drop {
  int value;
  ~Drop() noexcept { observe_drop(value); }
};

extern "C" int excluded_cleanup(int value) {
  Drop guard{value};
  return value + 7;
}

namespace inline_control {
inline int excluded_inline(int value) {
  return value - 4;
}
}

extern "C" int use_inline(int value) {
  return inline_control::excluded_inline(value);
}
