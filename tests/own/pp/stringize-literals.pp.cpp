// [cpp.stringize]/2: when the argument contains a string-literal or character-literal, each `"`
// and `\` in it is preceded by a `\`, including in encoding-prefixed and raw literals.
#define str(x) #x
str("abc");
str('a');
str('\'');
str("\n");
str("a\"b");
str("\\");
str('"');
str("  two  spaces  ");
str(u8"x");
str(L'y');
str(R"(raw)");
str(x "s" 'c' y);
