// [cpp.replace.general]/12, [cpp.subst]/2, /3: variadic macros in declarations: __VA_ARGS__
// forwarding an initializer list or a function's arguments, __VA_OPT__ adding a comma only when
// arguments are present, and #__VA_ARGS__ forming a string.
#define ARRAY(name, ...) int name[] = { __VA_ARGS__ };
#define SUM(first, ...) sum(first __VA_OPT__(,) __VA_ARGS__)
#define TEXT(...) #__VA_ARGS__
#define CALL(f, ...) f(__VA_ARGS__)
int sum(int a) { return a; }
int sum(int a, int b) { return a + b; }
int sum(int a, int b, int c) { return a + b + c; }
ARRAY(empty_ish, 0)
ARRAY(three, 1, 2, 3)
const char* words = TEXT(a, b, c);
const char* nothing = TEXT();
int zero() { return 0; }
int total() { return SUM(1) + SUM(1, 2) + SUM(1, 2, 3) + CALL(zero) + CALL(sum, 4, 5); }
