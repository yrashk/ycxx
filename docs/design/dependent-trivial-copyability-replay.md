# Dependent trivial copyability in replayed templates

The direct predicate checks a concrete operand against the corresponding C++ type property.
This step supports dependent operands written in an ordinary class template's base, including an
alias template-id there, and in a selected namespace-scope variable template initializer. Astra
identified these two existing replay paths for libycxx's `core/meta_base.hpp`. The extension
probe continues to return zero while general substitution and class facts remain partial.
[Scope decision](https://github.com/yrashk/ycxx/issues/218#issuecomment-7062981982)

At the definition, the predicate retains one canonical qualified type in a unary `VDepOp`.
Source analysis records the original item and token offset separately in `Program`. The key uses
the source location mapping, including the offset of a replayed default member initializer.
The structural value is shared across occurrences and contains no source or instantiation point.
Unlocated substitution can preserve a still-dependent operand. A concrete operand needs the
owning token replay so it can be checked at the right point.

A written dependent occurrence forces the existing base replay even when alias substitution
discards the argument from the resulting type. An initializer is re-read only for the selected
variable template definition. The replay checks a formerly dependent class element at the demand
point and reports at its original spelling. An occurrence whose written operand was already
concrete keeps the direct predicate's source completeness check. Both paths consume the same
class fact and fold the same canonical Boolean. [Alias templates](https://eel.is/c++draft/temp.alias)
· [Instantiation](https://eel.is/c++draft/temp.inst) · [Type property](https://eel.is/c++draft/meta.unary.prop)

Incomplete class elements violate the trait's domain. Unproved complete classes hit a compiler
limit. Both keep their located hard diagnostics. The failed instantiation records that it has
already diagnosed the trait, so the type or constant value that demanded it need not emit a
secondary diagnostic. A checked constraint remains a hard error when evaluation demanded this
value and failed, even if another candidate is viable. Evaluation still follows the existing
constraint short-circuit rules. Forming the operand type itself uses ordinary substitution and
its existing diagnostics. [Constraints](https://eel.is/c++draft/temp.constr.op)
· [Substitution](https://eel.is/c++draft/temp.deduct.general)

General dependent defaults, deduction and wider class classification remain in
[#218](https://github.com/yrashk/ycxx/issues/218). The stored pattern's value structure does not
provide a contextual diagnostic path for those uses.
