// #1099: a builtin's arity and parameter conversions are checked on the expanded arguments, at
// instantiation ([temp.variadic]/5, [expr.call]/7).
using Size = decltype(sizeof 0);

template <class... A> Size too_many(A... a) { return __builtin_strlen(a...); } //! error
template <class... A> Size too_few(A... a) { return __builtin_strlen(a...); } //! error
template <class... A> int too_few_with_written(A... a) { return __builtin_memcmp(a..., Size{1}); } //! error
template <class... A> double nullary(A... a) { return __builtin_inf(a...); } //! error
template <class... A> double not_a_number(A... a) { return __builtin_nan(a...); } //! error
template <class... A> void trap(A... a) { __builtin_trap(a...); } //! error
template <class... A> const void* find(A... a) { return __builtin_memchr(a...); } //! error
template <class... A> int probe(A... a) { return __builtin_constant_p(a...); } //! error
template <class... A> int* address(A&... a) { return __builtin_addressof(a...); } //! error
template <class... A> Size converted(A... a) { return __builtin_strlen(a...); } //! error

Size a = too_many("a", "b");
Size b = too_few();
int c = too_few_with_written("a");
double d = nullary(1);
double g = not_a_number();
void e() { trap(1); }
Size f = converted(1);
const void* h = find("a", 1);
int i = probe(1, 2);
int x, y;
int* j = address(x, y);
