// [cpp.subst]/1, /2, [cpp.stringize]/2: __VA_ARGS__ is macro-replaced like any parameter unless
// next to `#` or `##`; #__VA_ARGS__ stringizes the variable arguments with their commas, a single
// space after each comma that had whitespace and none where there was none.
#define ONE 1
#define all(...) __VA_ARGS__
#define str(...) #__VA_ARGS__
#define cat(a, ...) a ## __VA_ARGS__
#define fwd(...) all(__VA_ARGS__)
all(ONE, ONE);
str(ONE, ONE);
str(a,b);
str( a ,  b );
str();
cat(x, ONE);
cat(x, 1, 2);
cat(x);
fwd(ONE, 2, (3, 4));
