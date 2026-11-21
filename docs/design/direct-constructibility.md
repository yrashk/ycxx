# Direct constructibility for concrete operands

Astra's design for the first `__is_constructible` step starts with a concrete target type
and an optional sequence of concrete argument types. The spelling is needed by libycxx's
`core/meta_base.hpp`. Built-in registration supplies the parser's required prefix and
allows additional comma-separated type-ids. The preprocessor probe keeps returning zero
during this step. [Scope decision](https://github.com/yrashk/ycxx/issues/218#issuecomment-7063415574)

The operand domain is checked before selecting a result. Each type must be complete, cv
`void`, or an array of unknown bound. A known-bound array needs a complete element type.
A class check uses the original trait source context, including the position of a default
member initializer when it is re-read. Pointer and reference types do not require completion
of the type they designate. An enumeration without a fixed underlying type is incomplete
in its own enumerator list. An incomplete operand violates the trait's domain and retains
its diagnostic through substitution. [Type properties](https://eel.is/c++draft/meta.unary.prop)
· [Complete types](https://eel.is/c++draft/basic.types.general)
· [Enumerations](https://eel.is/c++draft/dcl.enum)

A function or cv `void` target gives false for every admitted argument sequence. With zero
arguments, a scalar target gives true and a reference target gives false. For an arithmetic
target and arithmetic or arithmetic-reference arguments, one argument gives true and two
or more give false. These cases can be decided from canonical types without creating a
variable or evaluating an initializer. [Initialization](https://eel.is/c++draft/dcl.init.general)

Other complete operand combinations and dependent operands remain compiler limits. A later
class query needs constructor and destructor selection with unrelated-context access under
the unevaluated, immediate-context rules. Dependent type packs need an owning substitution
path. Both are tracked in [#218](https://github.com/yrashk/ycxx/issues/218).
