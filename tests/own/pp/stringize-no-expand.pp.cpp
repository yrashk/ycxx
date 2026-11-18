// [cpp.subst]/1, [cpp.stringize]/2: a parameter preceded by `#` is replaced by the stringized
// argument as written, without macro replacement; going through a second macro that does not
// stringize first replaces the argument (the str/xstr idiom of the [cpp.scope] examples).
#define VALUE 42
#define EMPTY
#define f(x) <x>
#define str(x) #x
#define xstr(x) str(x)
#define both(x) x #x
str(VALUE);
xstr(VALUE);
str(f(1));
xstr(f(1));
str(EMPTY);
xstr(EMPTY);
xstr(VALUE EMPTY VALUE);
both(VALUE);
