// [class.member.lookup]: identical designated types merge across base subobjects.
// XFAIL: gcc GCC 16.2 diagnoses distinct inherited aliases for the same type as ambiguous.
struct Left { using T = short; };
struct Right { typedef signed short int T; };
struct Combined : Left, Right {
  using Published = T;
  int member() { return sizeof(T); }
};
static_assert(sizeof(Combined::Published) == sizeof(short));
