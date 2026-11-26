// XFAIL: ycxx diagnoses pointers whose pointee arrays or member pointers differ, and array or function sources (#1399)
// Each line holds whatever the answer is, so only ycxx's "not supported yet" diagnostic fails it:
// a silent true or false answer is an XPASS. With #1399, assert the answers Clang gives instead
// (true, true, true, false, true, true and true).
struct C {};
#define EITHER(...) (__VA_ARGS__ || !(__VA_ARGS__))
static_assert(EITHER(__is_convertible(int(*)[3], int(*)[])));
static_assert(EITHER(__is_convertible(int(*&)[3], int(* const&)[])));
static_assert(EITHER(__is_convertible(int C::**, const int C::* const*)));
static_assert(EITHER(__is_convertible(int(*)[3], long(*)[3])));
static_assert(EITHER(__is_nothrow_convertible(int(*)[3], int(*)[])));
static_assert(EITHER(__is_convertible(int[3], const int*)));
static_assert(EITHER(__is_convertible(void(), void(*)())));

int main() {}
