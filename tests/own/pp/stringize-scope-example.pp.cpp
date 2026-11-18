// [cpp.scope] (example 4), [cpp.stringize], [cpp.concat], [cpp.rescan]: stringizing arguments
// with literals, `##` with a macro argument, the str/xstr and glue/xglue idioms, and a
// self-referential object-like macro produced by pasting. (The example's #include line is left
// out; its argument is kept as an ordinary line, and the fputs line is broken after the
// multi-line invocation, since Clang and GCC write the tokens after one to different lines.)
#define str(s) # s
#define xstr(s) str(s)
#define debug(s, t) printf("x" # s "= %d, x" # t "= %s", \
 x ## s, x ## t)
#define INCFILE(n) vers ## n
#define glue(a, b) a ## b
#define xglue(a, b) glue(a, b)
#define HIGHLOW "hello"
#define LOW LOW ", world"
debug(1, 2);
fputs(str(strncmp("abc\0d", "abc", '\4') // this goes away
 == 0)
str(: @\n), s);
xstr(INCFILE(2).h)
glue(HIGH, LOW);
xglue(HIGH, LOW)
