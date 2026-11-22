template<class T> constexpr int sized(T x, int n = sizeof(x)) { return n; }
using Array = char[7];
using Function = int();
static_assert(sized<Array>(nullptr) == sizeof(char*));
static_assert(sized<Function>(nullptr) == sizeof(int(*)()));
