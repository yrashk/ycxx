# Native definitions for private static constexpr helpers (#1174)

A non-template class can call a private static constexpr helper from the constant-evaluated
arm of `if consteval` or `if !consteval` ([stmt.if]). Both arms and the helper's definition
are checked, and constant evaluation can execute the helper. The native IR keeps the runtime
edge. Collecting the helper as an unconditional native definition can still fail on operations
that runtime callers never execute, such as the dynamic array allocation in the pinned
libycxx `<stdexcept>` header.

Astra's design reuses the native reference walk from [#1097](https://github.com/yrashk/ycxx/issues/1097).
An eligible in-class definition is an ordinary private static constexpr function of a named
non-template class at namespace scope. It must have external coalesced inline emission
and have neither static locals nor closures. The object stage looks for a resolved direct
call to the helper in the checked AST of a constant-evaluated arm before requesting L1
bodies. Unrelated definitions remain available to other translation units that name their
symbols directly. A native direct call or address in lowered code or data demands the real
definition and follows its own native references. Definitions retain the existing weak
binding, including when two objects use their addresses.

The whole unit's declaration facts preserve emission for attributed definitions, including
attributes on another declaration of the same function. A unit with raw assembly names can
refer to another entity's symbol by spelling. Keep its existing emission roots, as in a
checked-policy build where policy object records may require a definition. Immediate functions
retain their separate emission rule. Runtime dynamic array allocation is tracked in
[#293](https://github.com/yrashk/ycxx/issues/293).
