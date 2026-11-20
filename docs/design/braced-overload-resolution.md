# Braced arguments in free function overload resolution

[#231](https://github.com/yrashk/ycxx/issues/231) starts with non-template free function
candidates. Astra checked the conversion rules and the boundary between ranking and
initializing the selected parameter.

Keep each braced list as written through name lookup and overload resolution. A typed
sibling argument can supply associated namespaces for ADL ([basic.lookup.argdep]). Rank a
braced argument as part of the same implicit conversion sequence comparison as its siblings
([over.ics.list], [over.ics.rank]). Check parameter count and conversions of the other
arguments before an unsupported list conversion can block the call.

- A by-value arithmetic parameter receives an identity sequence for an empty list. A single
  expression without a designator uses that expression's ordinary conversion sequence.
  Multiple, nested or designated clauses have no arithmetic conversion sequence.
- A complete ordinary aggregate can receive a flat positional list of arithmetic expressions
  through its public arithmetic fields. Each supplied field needs an implicit conversion.
  The whole list contributes one user-defined sequence with an identity second conversion.
  Field conversion ranks and narrowing do not decide the winner. The user-defined sequence
  ranks ahead of the ellipsis sequence.
- An unsupported conversion can affect the result if its candidate remains plausible. Report
  it at the list. The guarded cases include class-valued clauses, class templates, unions, bases,
  nonpublic fields, constructors, references, array and initializer-list parameters, and template
  candidates. A dependent clause defers analysis until instantiation.

Initialize only the selected function's arguments using the existing call path. It applies
[dcl.init.list] narrowing checks, builds the aggregate and initializes its omitted fields, and
diagnoses a list selected for an ellipsis. A sole known target continues to use this path
without ranking a list. Constructors, member overloads, template deduction and further list
conversion categories remain under #231.
