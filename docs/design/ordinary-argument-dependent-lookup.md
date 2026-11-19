# Ordinary argument-dependent lookup

[#165](https://github.com/yrashk/ycxx/issues/165) adds the namespace candidates from
[basic.lookup.argdep] to unqualified function calls. Astra independently checked lookup at
the template's lexical point, the exact namespace search, the overload merge, and the use
of the same lookup for the nonmember branch of [stmt.ranged].

A call of an unqualified-id can use ADL when ordinary lookup finds nothing or finds only
namespace-scope functions or function templates. Class members, functions declared in a
block, and nonfunctions suppress the search. A block using-declaration still names the
declaration it introduced. Qualified and parenthesized callees use ordinary lookup.
Merge function candidates by entity before overload resolution under [over.call.func].
Keep the ordinary callee if ADL changes no candidate or usable default arguments, and
query a deduced return type only after choosing its function.

The semantic context answers a separate question for functions named in an associated
namespace. Search the namespace that the declarations inhabit and the inline namespace set
of its innermost non-inline ancestor. A namespace using-declaration supplies the targets
visible at its own locus. Repeated declarations of an entity contribute the latest one that
this search finds. Ordinary qualified lookup may follow using-directives, including implicit
using-directives from unnamed namespaces. An ordinary call searches at its lexical
item, including that item where a body can see its own declaration. A nondependent call in a
template keeps that point during instantiation. For a dependent call, add the results at
the existing point of instantiation to the lexical results ([temp.dep.candidate]). Per-name
query-engine scheduling and execution counts remain under #16.

Compute associations from the argument types. Follow arrays, references and pointers to
their element or referred-to type, function types to parameters and return type, and
pointer-to-member types to the owner and member type. Classes and enums contribute their
nearest namespace. A complete ordinary class also contributes its direct and indirect bases.
Member-pointer association works on types in unevaluated operands (#238). Runtime values
remain under #411. Block-scope function declarations, exact operator lookup, class template
arguments, overload-set arguments and the complete unqualified template-id rules remain under
#165. Hidden friends are #259.

For a range-based `for`, an array supplies its own bounds and a class with both member names
supplies the member calls. The fallback forms the `begin` and `end` calls with an empty ordinary
lookup result, as [stmt.ranged] specifies.
