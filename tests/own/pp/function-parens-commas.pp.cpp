// [cpp.replace.general]/12: arguments are separated by commas, but commas between matching
// inner parentheses do not separate arguments; brackets and braces do not protect commas.
#define one(x) <x>
#define two(a, b) <a|b>
#define three(a, b, c) <a|b|c>
one((1, 2));
one((a, (b, c), d));
one(());
one(f(x, y));
two((1, 2), 3);
two(x[1], {y});
two([a, b]);
three((,), (,), (,));
two(one(1), one((2, 3)));
