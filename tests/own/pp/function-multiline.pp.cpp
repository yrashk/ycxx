// [cpp.replace.general]/12: within the sequence of preprocessing tokens making up an invocation
// of a function-like macro, new-line is considered a normal white-space character, so an
// invocation may span several lines, and the lines after it keep their own tokens. (Each
// multi-line invocation here ends its line: Clang and GCC differ on which line the tokens that
// follow the closing `)` on the same line are written to.)
#define f(a, b) [a b]
#define g(x) {x}
begin f(1,
  2)
after
f(
  x
  ,
  y
)
end;
g(g(1)
)
g(2) next
last;
