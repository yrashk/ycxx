# Presumed lines and function defaults

The zero-argument GNU builtin `__builtin_LINE()` returns an `unsigned int`
constant. A direct call uses its presumed source line ([cpp.line]). An
omitted ordinary function argument evaluates the default with the invocation's
line ([dcl.fct.default], [expr.call]). Nested omitted arguments inherit that
line while the default is evaluated. A function body invoked from a default
has its own lexical source context.

The profile follows Clang's type and line choice. GCC 16.2 and Clang 23.1
agree on the pinned assertion helper's ordinary default use. They differ on
multiline calls and macros. For a written call, Clang uses the callee's
starting line. A call through a written `.` or `->` uses the member name's
line. For tokens expanded by a function-like macro, it uses the closing
line of the invocation.
[#327](https://github.com/yrashk/ycxx/issues/327)

The flattened preprocessing result preserves each occurrence's `#line`
adjustment through includes. If a fully expanded token names the builtin,
the semantic projection also carries the presumed line for each token.
Defaults can then observe call sites elsewhere in the translation unit.
The projection contains only line numbers. Units without the builtin retain
the location-free cutoff from #947. A line-only edit in a unit with the
builtin can re-run analysis even if the edited line is never observed.

Written defaults retain a typed expression for the builtin. The omitted
argument binds a line at its call site. Both interpreter and native execution
use the same IR generator's scoped constant while generating the default.
An immediate invocation that observes the default's call-site line is
diagnosed until immediate folding can use the omitted argument's invocation
context. The check skips untaken conditional arms and short-circuit operands
when an integral literal determines control flow. Other controls are checked
conservatively without evaluating them during the check.
GNU constantness probes use the lexical line outside defaults. A probe whose
proof needs an omitted argument's invocation line gives zero while that line
is unresolved [#1065](https://github.com/yrashk/ycxx/issues/1065).
This follows Astra's review of the location projection and the scope of an
omitted default. The full library facility is tracked in
[#327](https://github.com/yrashk/ycxx/issues/327).
