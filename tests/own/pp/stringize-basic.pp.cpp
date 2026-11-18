// [cpp.stringize]/2: `#` followed by a parameter becomes a single string-literal spelling the
// argument's tokens: whitespace between tokens becomes one space, whitespace before the first
// and after the last token is dropped, and an empty argument gives "". (A new-line inside the argument counts as whitespace;
// the invocation ends its line since Clang and GCC write the following `;` to different lines.)
#define str(x) #x
#define str2(x) # x
str(hello);
str2(hello);
str(  a   b  );
str(a+b);
str(a + b);
str(a
    b)
;
str();
str( );
str(f(x, y));
str((a, b));
str(1.5e+3);
str(<:);
str(::);
