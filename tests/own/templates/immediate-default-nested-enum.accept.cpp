enum E { one = 1 };
consteval E pick(int) { return one; }
namespace table { template<E> constexpr unsigned mapped = 7; }
template<int N, unsigned A = table::mapped<pick(N)>> struct Result;
extern Result<2>* value;
extern Result<2, 7>* value;
