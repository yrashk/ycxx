// #257: a static assertion may appear among the member declarations
// and refer to an earlier member ([class.mem.general], [dcl.pre]).
struct Record {
  static constexpr int minimum = 1;
  static_assert(minimum > 0);
  int value;
};

Record record{1};
