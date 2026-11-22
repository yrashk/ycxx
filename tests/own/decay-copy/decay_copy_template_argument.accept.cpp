template<int N>
struct Tag {};

extern Tag<7> parens;
extern Tag<auto(7)> parens;

extern Tag<7> braces;
extern Tag<auto{7}> braces;
