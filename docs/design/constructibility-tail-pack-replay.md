# Constructibility with a tail pack

Astra's design extends the direct predicate to the written forms in libycxx's
`core/meta_base.hpp`. The builtin has one mandatory written target type-id. Its argument
operands can include type pack expansions, with ordinary operands on either side of an
expansion. The existing base and variable initializer replay paths own the dependent query.
The feature probe continues to return zero. [Scope](https://github.com/yrashk/ycxx/issues/218)
· [Direct predicate](direct-constructibility.md)

Each operand contributes one canonical type argument to the predicate's structural value.
For an expansion, the type argument wraps the pattern in the existing pack-expansion type.
Substitution splices the successful expansion at its written position. An empty expansion
contributes zero arguments and preserves the target. An unbound expansion remains dependent.
A failed expansion stops substitution. The parser also records the need to replay a base
argument when an alias discards the resulting Boolean. [Pack expansions](https://eel.is/c++draft/temp.variadic)
· [Alias templates](https://eel.is/c++draft/temp.alias)

The source occurrence records which written operands were dependent, using the owning item and
token position. All elements of an expanded operand share that position. At replay, those
operands use the owner's demand for completeness. A concrete operand keeps its source context,
including the original position of a default member initializer if its tokens are read again.
The diagnostic identifies the builtin's original location. When a nested demand has already
diagnosed an operand outside the domain or a compiler limit, the base or variable instantiation
retains that failure through constant evaluation and constraint checking.
[Instantiation](https://eel.is/c++draft/temp.inst) · [Type properties](https://eel.is/c++draft/meta.unary.prop)

The checked operand sequence uses the direct predicate's existing computations. Ordinary
operand type formation and pack-length errors retain substitution behavior. General dependent
defaults, deduction and class construction remain in [#218](https://github.com/yrashk/ycxx/issues/218).
