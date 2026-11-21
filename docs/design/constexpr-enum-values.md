# Scalar enumeration values in constant evaluation (#262)

An enumeration's L1 values and object places retain their nominal C++ types. Verified L1
already represents a constant, parameter, return, conversion or comparison of enumeration type.
The interpreter currently rejects that constant because its scalar shape recognizes only
builtin integer types. A function parameter and a local also need a scalar cell of the same
nominal type. Resolve the underlying type from the evaluation's existing enumeration records
and use its width and signedness for the integer payload. The interpreter's type checks still
compare nominal types. Sema still decides which conversions and comparisons are legal.

Keep AST integer constants in their existing underlying-type encoding. Convert the interpreter's
typed result to that encoding when interning it. The declaration, expression, access path and L1
result retain the enumeration type, and an enum cell in a constexpr array uses the same integer
encoding. A static constexpr enum member therefore uses the scalar load path. Class-valued
allocation and materialization are a distinct [#262](https://github.com/yrashk/ycxx/issues/262)
slice under the interpreter umbrella [#25](https://github.com/yrashk/ycxx/issues/25).

Fixed enum casts follow the underlying type's conversion. For a non-fixed enum, check an
explicit cast against the range of the hypothetical minimum-width integer type in [dcl.enum].
Check the original integral value before narrowing to the storage type. A cast from floating
point first converts to the underlying type. Keep a non-fixed cast in L1 even when its operand
is a literal so constant evaluation can reject an out-of-range value on an evaluated path.
The check applies to constant evaluation, and native lowering keeps its existing representation.
[[expr.static.cast]](https://eel.is/c++draft/expr.static.cast) ·
[[dcl.enum]](https://eel.is/c++draft/dcl.enum) · [[expr.const]](https://eel.is/c++draft/expr.const)

The regression cases cover enum arguments and returns, local and static scalar storage,
switch dispatch, signed narrow and high unsigned casts, fixed and non-fixed ranges, and
non-constant execution. The same-type comparisons and invalid scoped promotions exercise
the boundary between nominal type checking and the integer payload. Run the fixtures through
both target front ends and the available native run paths.
