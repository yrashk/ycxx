consteval int choose(int*, int) { return 1; }
consteval int choose(long, long) { return 2; }
template<int N, int A = choose(N, 1)> struct Result;
extern Result<3>* from_three;
extern Result<3, 2>* from_three;
extern Result<0>* from_zero;
extern Result<0, 2>* from_zero;
static_assert(choose(0, 1) == 1);
static_assert(choose(nullptr, 1) == 1);
