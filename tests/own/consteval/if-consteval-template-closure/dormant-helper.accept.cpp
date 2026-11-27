// [stmt.if], [temp.inst], [dcl.pre]
struct WideTag {
  unsigned char bytes[2];
};

template<class T>
constexpr int helper(int value) {
  static_assert(sizeof(T) == 1, "helper requires a one-byte type");
  return value;
}

template<class Tag>
constexpr int unused_bridge(int value) {
  if consteval {
    return helper<Tag>(value);
  }
  return value;
}

extern "C" int deferred_pattern_anchor() {
  return 37;
}
