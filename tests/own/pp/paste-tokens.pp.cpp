// [cpp.concat]/3: `##` between two tokens of a replacement list concatenates the token before it
// with the token after it (the first or last token of an argument, for a parameter), forming
// identifiers, pp-numbers, operators and punctuators; whitespace around `##` does not matter.
#define cat(a, b) a ## b
#define cat2(a, b) a##b
#define prefix(x) prefix_ ## x
#define suf(x) x ## _suffix
cat(foo, bar);
cat2(foo, 123);
cat(1, 2);
cat(1, e);
cat(0x, 1F);
cat(1.5, e);
cat(., 5);
prefix(name);
suf(name);
cat(+, +);
cat(-, >);
cat(<, <=);
cat(-, =);
cat(&, &);
cat(:, :);
cat(%:, %:);
cat(<, :);
cat(L, 'a');
cat(u8, "s");
cat(a b, c d);
cat(_, _);
