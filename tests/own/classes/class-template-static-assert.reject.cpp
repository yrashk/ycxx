// #257: defining an object requires a complete class specialization,
// so its dependent assertion is checked ([temp.inst], [dcl.pre]).
template<int Count> constexpr bool positive_count = Count > 0;

template<int Count> struct CheckedCount {
  static constexpr int count = Count;
  static_assert(positive_count<Count> && count <= 4, "count must be between 1 and 4"); //! error
};

CheckedCount<5> out_of_range;
