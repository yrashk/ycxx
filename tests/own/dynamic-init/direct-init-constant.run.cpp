// ι (#94), from the review: `T x(args)` with a constant initializer is constant-initialized, and
// a constexpr or const integral variable so initialized is usable in constant expressions
// ([basic.start.static], [expr.const]).
// EXIT: 0
constexpr int k(5);
static_assert(k == 5);
const int c(3);
int arr[c];
int main() { return sizeof(arr) == 3 * sizeof(int) ? 0 : 1; }
