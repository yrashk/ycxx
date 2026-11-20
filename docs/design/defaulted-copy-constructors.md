# First-declaration defaulted copy constructors

A first-declaration in-class defaulted copy is user-declared and non-user-provided. Its
constructor entity and parameter type stay in the overload set. It suppresses the implicit
default constructor, prevents aggregate initialization, and suppresses implicit move
construction. The exact `= default;` item body is a definition. Body analysis and code emission
use the selected copy operation. [[dcl.fct.def.default]](https://eel.is/c++draft/dcl.fct.def.default) ·
[[class.copy.ctor]](https://eel.is/c++draft/class.copy.ctor)

The supported declarations have one `C&` or `const C&` parameter with no default argument, in an
ordinary namespace-scope class. They permit `constexpr` and a bare `noexcept`. Classification
checks subobjects from the corresponding source glvalue, including their cv-qualifiers,
reference members, and array elements. A private copy in a member is checked with the
enclosing constructor's access. Known deletion is diagnosed when overload resolution
selects the constructor. An implicit wrapper whose copy depends on a newly defaulted copy
uses the same classification. [[class.copy.ctor]](https://eel.is/c++draft/class.copy.ctor)

For classes without bases, virtual functions, or unions, a proven trivial selected copy uses
the existing representation-copy IR. The member's original entity is checked for access
before that lowering. Nontrivial and unresolved operations receive a diagnostic at use.
An unresolved classification also prevents native by-value parameter and result lowering.
The Itanium and Apple predicates for known operations follow DECISIONS §1.6.
[Itanium C++ ABI §3.1.2.3](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#non-trivial-parameters)

Astra reviewed the declaration, selection, and ABI boundaries. Additional defaulting and
nontrivial copies remain in #157. Inline coalescing, class constant evaluation, exception
specifications, assignment, and bases are tracked in #124, #25 and #102, #219, #244 and #360,
and #308.
