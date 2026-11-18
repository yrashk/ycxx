// [expr.const], [dcl.constexpr]: calling a non-constexpr function, reading a global that is not
// usable in constant expressions, or modifying a global is not a constant expression. Each
// offending operation is on its marked line.
int plain(int x) { return x + 1; }
int counter = 0;
const int fixed = 7;
constexpr int also_fixed = 8;
constexpr int reads_fixed() { return fixed + also_fixed; }
constexpr int calls_plain(int x) { return x > 0 ? plain(x) : 0; } static_assert(calls_plain(1)); //! error
static_assert(calls_plain(0) == 0);
static_assert(reads_fixed() == 15);
static_assert(fixed == 7);
static_assert(plain(1) == 2); //! error
static_assert(counter == 0); //! error
constexpr int from_counter = counter; //! error
constexpr int from_plain = plain(2); //! error
constexpr int reads_counter() { return counter; } static_assert(reads_counter() == 0); //! error
constexpr int bump() { counter = counter + 1; return 1; } static_assert(bump() == 1); //! error
constexpr int bump_pre() { return ++counter; } static_assert(bump_pre() == 1); //! error
