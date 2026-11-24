// #257: check a dependent assertion when its class specialization is
// instantiated ([dcl.pre], [temp.inst]).
template<int Count> constexpr bool positive_count = Count > 0;

template<int Count> struct CheckedCount {
  static constexpr int count = Count;
  static_assert(positive_count<Count> && count <= 4, "count must be between 1 and 4");
};

CheckedCount<5>* pointer_only = nullptr;
static_assert(sizeof(CheckedCount<1>) > 0);
CheckedCount<4> at_limit;
