# ycxx

**An independent, resident C++26 compiler.**

ycxx is being built to compile C++26 from source to linked executables by itself: preprocessor,
front end, constant evaluator, code generator, object writer and linker. It is designed not to
run once per translation unit and exit, but to stay resident, hold the whole program's analysis
in memory, and, when a file changes (reported by a file-system watcher, an editor or a build
tool), recompute only what that change can affect.

## Watch mode demo

Vim edits trigger live rebuilds in ycxx's interactive trace. The demo follows one function through
L1 IR, lowered L2 IR, and emitted ARM64 instructions, then shows compiler errors and their fixes.
The final Linux x86-64 self-check uses the freestanding support in [#380](https://github.com/yrashk/ycxx/pull/380).

![Vim and ycxx watch mode showing live rebuilds, L1 and L2 IR, and ARM64 assembly](docs/media/ycxx-watch-ir-codegen.gif)

[View the GIF at full size](docs/media/ycxx-watch-ir-codegen.gif)

## Status

**Pre-0.1** (as of 2026-11-22). ycxx compiles a substantial subset of C++ (C-like code, classes,
templates and constant evaluation) on macOS arm64, from source to a signed, running executable,
with no external compiler, assembler or linker. It also builds static executables from a
freestanding C++ subset for x86-64 Linux. The standard library is out of reach: the gaps on
`<iostream>`'s path are mapped in [#776](https://github.com/yrashk/ycxx/issues/776). The plan is in
[DECISIONS.md](DECISIONS.md), the details of the code in [STATUS.md](STATUS.md), and the work in
[GitHub issues](https://github.com/yrashk/ycxx/issues) and the
[roadmap](https://github.com/yrashk/ycxx/issues/12).

### What you can do today

Ordinary function pointers and references preserve concrete `noexcept` type identity.
A nonthrowing function can be used where the corresponding potentially throwing type is expected.
The unary `noexcept` operator checks whether calls and constructions can throw.
Member function template specializations preserve bare `noexcept` in their function types.
[#729](https://github.com/yrashk/ycxx/issues/729)
Ordinary constructor declarations and definitions check that their concrete exception specifications agree.
Function templates, member function templates and members of class templates substitute a
dependent exception specification when it is needed.
[#713](https://github.com/yrashk/ycxx/issues/713) [#828](https://github.com/yrashk/ycxx/issues/828)
A lambda's noexcept-specifier, including one that depends on an enclosing template's
parameters, is part of its call operator's type. [#807](https://github.com/yrashk/ycxx/issues/807)

A class template can grant private access to the class passed as a type argument using
`friend Authority;`. Each specialization grants access to its own authority.
[#689](https://github.com/yrashk/ycxx/issues/689)

**Compile and run programs on macOS.** `ycxx cxx a.cpp -o a` (also `-c`, `-fsyntax-only`, `-E`, `-I`,
`-D`, `-std=c++26`, and `-fclass-call-abi=apple` to pass classes as Apple Clang does) builds an executable with ycxx's own front end, IR, AArch64 code generator
and Mach-O linker on arm64 macOS, where `arm64-apple-darwin` is the default target. It also links
object files from Clang or GCC, and static archives (`.a`) made by the system `ar`. Programs
link libycxx's runtime by default, so `<cstdio>` works (`-nostdlib++` leaves it out)
([#842](https://github.com/yrashk/ycxx/issues/842)). Variadic functions can be defined with `<cstdarg>` or `<stdarg.h>` on both
macOS and Linux ([#647](https://github.com/yrashk/ycxx/issues/647)). Errors are reported as
`file:line:col: error: …`.
Errors during replay of a written default member initializer point to the initializer.
[#806](https://github.com/yrashk/ycxx/issues/806)

The arm64 Mach-O linker preserves supported cleanup-free and cleanup-only DWARF frames from input objects.
[Mach-O unwind support #72](https://github.com/yrashk/ycxx/issues/72)
It resolves PC-relative GOT words at nonzero offsets in independently assembled objects.
[#782](https://github.com/yrashk/ycxx/issues/782)

```sh
tools/build
build/bin/ycxx cxx hello.cpp -o hello && ./hello
```

`tools/build` builds ycxx in Debug. A Release build (`YCXX_BUILD_TYPE=Release
YCXX_BUILD_DIR=build-release tools/build`, [docs/BUILDING.md](docs/BUILDING.md#release-builds))
builds and passes its tests on both hosts, and compiles a 40,000-statement function in 0.4 s
instead of Debug's 5.3 s on an M-series Mac ([#726](https://github.com/yrashk/ycxx/issues/726)).

Single-expression `auto(x)` and `auto{x}` perform scalar and pointer decay-copy, including
array and function decay. Non-type template parameters can have enumeration types
([#774](https://github.com/yrashk/ycxx/issues/774)). Function-template bodies can defer supported dependent operands to
instantiation, and a function template whose signature holds a dependent variable-template-id
or its negation compiles to an object ([#773](https://github.com/yrashk/ycxx/issues/773)). C++26
pack-index-specifiers `T...[I]` select a type from a pack ([#297](https://github.com/yrashk/ycxx/issues/297);
the expression form `t...[I]` is not done yet). Concrete constant template arguments can use braced conversions such as
`Tag<auto{7}>` and integer-literal-led parenthesized conversions such as `Tag<auto(7)>`.
[#286](https://github.com/yrashk/ycxx/issues/286)

Functional conversions can construct namespace-qualified class-template specializations with
parentheses or braces, including through namespace aliases. [#697](https://github.com/yrashk/ycxx/issues/697)

Class templates at namespace scope can delete their copy assignment on its first in-class
declaration when the member has no ref-qualifier. Specializations retain the deleted overload.
[#709](https://github.com/yrashk/ycxx/issues/709)

In a class template, and in a member defined outside it, a member of a nondependent base hides a
template parameter of the same name. [#800](https://github.com/yrashk/ycxx/issues/800)

C++26 reflection annotations on class heads and ordinary function declarations accept
constant payloads and retain their types for compiler consumers.
[#662](https://github.com/yrashk/ycxx/issues/662)

**Preprocess binary resources.** Both targets support `#embed` and `__has_embed` with the standard
parameters. [#125](https://github.com/yrashk/ycxx/issues/125)

**Compile for x86-64 Linux.** A ycxx build for x86-64 Linux defaults to `x86_64-pc-linux-gnu`
and uses the System V AMD64 backend. `--target` overrides either host default. A build with no
supported host default requires an explicit target. Use `-c` to write an ELF object or
`-nostdlib` to link C++ sources, ycxx-generated `.o` files, or a mixture into a static executable.
The startup runs namespace-scope
initializers, then calls `int main()` or `int main(int, char**)` and passes its return value to Linux
as the exit status. A program without such initializers can supply its own `_start`. Given
`example.cpp` containing
`int main() { return 0; }`, run the following on x86-64 Linux.

```sh
build/bin/ycxx cxx -c example.cpp
build/bin/ycxx cxx -nostdlib example.o -o example
./example
```

Fixed-prototype calls (free and member functions, lambdas, function templates and calls
through pointers) support passing and returning nonempty flat records with integral or
ordinary object-pointer fields by value. The record can occupy up to
16 bytes and must be trivial for calls. Larger results and classes that are not trivial for
calls (with a user-provided copy constructor or destructor) pass through memory, as on macOS. [Linux integer aggregate ABI](docs/design/linux-integer-aggregates.md)

For mixed Linux objects, the pinned libycxx `<cstddef>` byte operator uses the same Itanium
linkage name in ycxx, GCC and Clang. [#64](https://github.com/yrashk/ycxx/issues/64)
Static links of explicit x86-64 ELF objects keep the first COMDAT group for each signature, including
its code, data and relocations. [#422](https://github.com/yrashk/ycxx/issues/422)

**Package Linux objects into a static library.** `ycxx archive -o libexample.a example.o`
creates or replaces a deterministic indexed GNU archive from supported x86-64 ELF objects.
Static links accept these archives and compatible archives from system `ar`. Place an archive
after the inputs that reference its members, and include at least one source or ordinary object.
[#389](https://github.com/yrashk/ycxx/issues/389), [#388](https://github.com/yrashk/ycxx/issues/388)

An explicit dynamic link uses ycxx's startup and the target loader. Supply the absolute target
interpreter path and explicit, self-contained shared objects with GNU or SysV hash tables and
slash-free SONAMEs. The executable can directly call their exported, unversioned functions. The
loader must be able to find each SONAME when the executable runs. [#390](https://github.com/yrashk/ycxx/issues/390)

```sh
build/bin/ycxx cxx --target=x86_64-pc-linux-gnu -nostdlib --dynamic-linker="$target_loader" \
  example.cpp "$target_library" -o example
```

Linux objects pass and return `bool`, 8-, 16-, 32- and 64-bit integers and enums, pointers and
references. Direct and function-pointer variadic calls accept promoted integer and pointer
arguments. Ordinary free variadic definitions with scalar INTEGER and SSE signatures support
`<cstdarg>`, including `va_arg(ap, double)`. A System V `va_list` can be passed between ycxx
and separately compiled code.
Use the host linker to link these objects with libc.
[#417](https://github.com/yrashk/ycxx/issues/417), [#412](https://github.com/yrashk/ycxx/issues/412),
[#399](https://github.com/yrashk/ycxx/issues/399), [#196](https://github.com/yrashk/ycxx/issues/196)

Fixed-prototype definitions and direct calls on x86-64 Linux also move `float` and `double`
arguments and results, directly and through function pointers. Variadic calls support the same
scalar floating values and promote an unnamed `float` to `double`. Direct call result types can
be deduced from `auto` or `decltype(auto)`. Runtime addition, subtraction, multiplication,
division and unary negation work for `float` and `double` at their evaluation width. Both types
support the six comparisons, conversion to `bool`, conversions to and from every integer type,
`double` to `float`, and `__builtin_fabs`. [#643](https://github.com/yrashk/ycxx/issues/643)
[Linux scalar floating-point ABI](docs/design/linux-scalar-floating-point.md) ·
[Linux scalar floating-point operations](docs/design/linux-scalar-floating-point.md#runtime-arithmetic) ·
[Linux scalar variadic floating-point ABI](docs/design/linux-floating-variadic-calls.md) ·
[#441](https://github.com/yrashk/ycxx/issues/441)

On x86-64 Linux, the System V indirect result convention also covers an ownership wrapper
whose by-value member makes its implicit destructor nontrivial. The returned object is
constructed in the caller's storage. [#661](https://github.com/yrashk/ycxx/issues/661)

ELF and Mach-O objects support static pointer initializers naming variables, array elements,
or functions. The referenced symbol may be defined in a separate translation unit.
[#101](https://github.com/yrashk/ycxx/issues/101)

Both targets support UTF-16 and UTF-32 string literals (`u` and `U`), including raw strings,
constant evaluation, and initialization of matching character arrays.
[#288](https://github.com/yrashk/ycxx/issues/288)

Ordinary namespace-scope references to supported scalar and array storage can bind at compile time
or during ordered startup initialization. On Linux, generated code can access bindings and
referents with internal linkage. [#367](https://github.com/yrashk/ycxx/issues/367)

Linux x86-64 programs can read and write data and take the address of data and ordinary
functions defined in another translation unit. Native static executables and host-linked objects
support those addresses. [#414](https://github.com/yrashk/ycxx/issues/414)
· [#407](https://github.com/yrashk/ycxx/issues/407)

On both targets, direct calls and saved pointers across translation units share one address for
simple external inline functions, with ycxx's linker or the system's. Ordinary namespace-scope
functions can use GNU hidden visibility, including coalesced inline and `constexpr` definitions.
On Linux x86-64, ordinary external non-inline namespace variables also preserve GNU hidden
visibility across declarations.
[#422](https://github.com/yrashk/ycxx/issues/422) · [#602](https://github.com/yrashk/ycxx/issues/602)
· [#124](https://github.com/yrashk/ycxx/issues/124) · [#936](https://github.com/yrashk/ycxx/issues/936)

External inline namespace variables with relocation-free static initializers share one object
across translation units on both targets. On Linux this lets two freestanding units using libycxx
`<cstddef>` link with the pinned static library. [#432](https://github.com/yrashk/ycxx/issues/432)

ycxx ELF objects record function types and code sizes. The ELF linker accepts supported
system-assembled objects with function symbols and the GNU-built libycxx static linkage-marker
object. Symbol names can share suffixes in an input string table.
[#428](https://github.com/yrashk/ycxx/issues/428), [#419](https://github.com/yrashk/ycxx/issues/419),
[#484](https://github.com/yrashk/ycxx/issues/484)

On x86-64 Linux and arm64 macOS, an exception thrown by hosted peer code can unwind through
eligible cleanup-free ycxx functions with fixed scalar parameters and reach a handler in peer code.
This includes calls through function pointers with fixed scalar prototypes, calls in selected default
member initializers of automatic scalar aggregates, and calls while producing trivial scalar
aggregate results through the target's supported ABI.
[#572](https://github.com/yrashk/ycxx/issues/572) · [#737](https://github.com/yrashk/ycxx/issues/737) ·
[#849](https://github.com/yrashk/ycxx/issues/849) · [#851](https://github.com/yrashk/ycxx/issues/851) ·
[#853](https://github.com/yrashk/ycxx/issues/853)

On x86-64 Linux, an eligible ycxx function can also throw an `int` to a hosted peer's handler.
A host-linked ycxx function can use one `catch (...)` to return an integer literal when
a direct call on its parameter to an externally defined `int(int)` function throws.
It can also use one `catch (int value)` to return the caught integer and let exceptions of
other types reach the hosted caller.
A supported union constructor can destroy its active member when its body call throws and
propagate an exception from its initializer before the member's lifetime begins.
[#578](https://github.com/yrashk/ycxx/issues/578)
· [#583](https://github.com/yrashk/ycxx/issues/583) · [#919](https://github.com/yrashk/ycxx/issues/919)
· [#738](https://github.com/yrashk/ycxx/issues/738)

On both hosts, a system-linked void function can destroy one initialized aggregate local
on both exits from an external `void()` call and let the original exception reach a peer handler.
[#663](https://github.com/yrashk/ycxx/issues/663) · [#781](https://github.com/yrashk/ycxx/issues/781)

Both targets support cooked integer user-defined literals through namespace-scope literal
operators with an `unsigned long long` parameter, including constant evaluation.
[#289](https://github.com/yrashk/ycxx/issues/289)

Both targets support typed constant addresses and reads within named namespace-scope constexpr
arrays of supported scalar types, including arithmetic on saved pointers just past a nested row.
[#111](https://github.com/yrashk/ycxx/issues/111)

Both targets support `constexpr` constructors in class definitions and plain out-of-class
definitions for runtime construction. Ordinary in-class constructor declarations support
bare `explicit`. An ordinary class can explicitly default its copy constructor as its first
declaration and use it for trivial member copies. A non-template empty tag class at namespace
scope with no bases and an implicit trivial destructor can first declare `T() = default`
in class and use it to initialize an `inline constexpr` tag object.
[#157](https://github.com/yrashk/ycxx/issues/157)

An explicit class return can select a written move constructor for an eligible named local
or parameter. [#749](https://github.com/yrashk/ycxx/issues/749)

An ordinary call returning a class by value preserves the result's cv-qualification for
`decltype` and overload resolution. [#679](https://github.com/yrashk/ycxx/issues/679)

An ordinary class object can be destroyed explicitly through a reference or pointer with
`object.~Class()` or `pointer->~Class()`. Templates can use a dependent class name, and unary
`noexcept` uses the destructor's exception specification. [#722](https://github.com/yrashk/ycxx/issues/722)

Both targets construct a class prvalue selected by the built-in conditional or comma operator
directly into an automatic variable, its ordinary aggregate field, or its fixed-array element.
The supported nonthrowing initializers keep temporaries alive through later initializer clauses
and destroy them in reverse order. [#740](https://github.com/yrashk/ycxx/issues/740)

In-class conversion functions can name a class type alias as their conversion target,
including a dependent alias in a class template. A named `static_cast` can call a
conversion function for an exact non-class value type, including ordinary `&`- and
`&&`-qualified conversion functions. [#339](https://github.com/yrashk/ycxx/issues/339)

Both targets accept leading `[[…]]` attributes on ordinary data and function member declarations.
[#190](https://github.com/yrashk/ycxx/issues/190)

Ordinary named namespace-scope classes and structs support class-head `alignas` requests up to
16 bytes. The alignment applies to layout and storage on both targets. [#303](https://github.com/yrashk/ycxx/issues/303)

Both targets accept nonempty standalone unnamed enum definitions at namespace scope, including
fixed underlying types and definitions inside `extern "C"` blocks.
[#157](https://github.com/yrashk/ycxx/issues/157)
Nonempty standalone unnamed enum definitions introduce their enumerators into the enclosing
class. Class template specializations support the same form with nondependent enumerator
values. [#859](https://github.com/yrashk/ycxx/issues/859)

Qualified value lookup can read earlier static data members and injected unscoped enumerators
while their current class is incomplete. [#857](https://github.com/yrashk/ycxx/issues/857)

Both targets support named nested enum definitions followed by ordinary fields in the same
member declaration, including scoped enum field initializers. [#500](https://github.com/yrashk/ycxx/issues/500)
An enum with a fixed underlying type keeps that layout while its enumerators instantiate other
enum definitions. [#897](https://github.com/yrashk/ycxx/issues/897)
Scoped enumerators stay in their enumeration's scope, so class members and class template
members may reuse their names. [#725](https://github.com/yrashk/ycxx/issues/725)
Nested unscoped enumerators conflict with other non-type members regardless of declaration order.
They can hide an enum name in ordinary lookup while leaving it available as a qualifier.
[#731](https://github.com/yrashk/ycxx/issues/731)
Qualified elaborated specifiers such as `enum Box::State` find the enum despite a same-name
enumerator. [#761](https://github.com/yrashk/ycxx/issues/761)
Initializers in a nested enumeration can use preceding enclosing class members by unqualified
name. Earlier enumerators take precedence over enclosing class names within their enumeration.
Unevaluated operands can name a preceding non-static data member without an object, including
`sizeof(member)` in an initializer and `decltype(member)` as an underlying type. A call in an
unevaluated operand can select a preceding static member from an overload set.
[#760](https://github.com/yrashk/ycxx/issues/760) · [#882](https://github.com/yrashk/ycxx/issues/882)

Automatic declarations of ordinary non-template classes with only scalar fields and no bases
or unions can use default member initializers in `S s;` and `S s{…};`. A default runs in
declaration order and can call a function or read an earlier field through `this`. An explicit
initializer takes precedence. The same rules apply to ordinary non-inline namespace objects,
whose dynamic initialization runs in definition order.
[#118](https://github.com/yrashk/ycxx/issues/118)

An out-of-line constructor evaluates a default member initializer with the namespace names
visible where the initializer was written and lookup in the complete enclosing class.
[#764](https://github.com/yrashk/ycxx/issues/764)
Later class members can also hide namespace names in defaults that evaluate to scalar constants.
[#763](https://github.com/yrashk/ycxx/issues/763)
YCXX checks the non-dependent parts of a written default member initializer once the class is
complete, even if no initialization selects that default.
[#732](https://github.com/yrashk/ycxx/issues/732)
In non-template code, ordinary scalar aggregates evaluate written defaults in temporaries such
as `S{4}`, empty value-initialization such as `S()`, return values, and by-value arguments.
These runtime defaults can use built-in scalar operations and ordinary nonthrowing calls,
including reads of earlier fields. Required constant aggregate initialization also uses the
completed class to resolve scalar defaults.
[#818](https://github.com/yrashk/ycxx/issues/818)
Function templates also use the completed-class defaults when initializing eligible ordinary
scalar aggregate types. [#846](https://github.com/yrashk/ycxx/issues/846)

Automatic declarations such as `Point p(3, 4);` can initialize flat aggregates with public
arithmetic fields. Supplied expressions initialize the fields in declaration order. Omitted
fields use their default member initializer when available and otherwise get zero. A runtime
declaration can also initialize one flat public direct base from an exact-type lvalue or an
unqualified braced prvalue such as `B{7}`. Omitted trailing arithmetic fields of that aggregate
can use call-free default member initializers that read the initialized base. Fields with no
written default member initializer are value-initialized.
[#301](https://github.com/yrashk/ycxx/issues/301)

**Language on macOS.** Programs can use:
- the fundamental integer, character and floating types, pointers, arrays, references and
  enumerations, including opaque declarations of named enums in their namespaces; `float`,
  `double` and `long double` with literals, arithmetic, comparisons and
  conversions giving Clang's results bit for bit, and passing and returning them (classes of
  floating members included) by Clang's calling convention;
  equality and ordering of pointers to matching complete arrays whose elements differ in
  `const` or `volatile` qualification. Comparisons and conditional expressions involving
  object pointers and `void` pointers combine the pointee qualifiers. Statements include
  `switch`, labels and `goto` (including jumps past vacuous initialization), declarations in conditions
  including `if constexpr`, init-statements in `if` and `switch`, and range-based `for` with optional
  init-statements.
  Ranges can be arrays or classes with member `begin` and `end` or free `begin` and `end` found
  by argument-dependent lookup. Unqualified calls can find and rank functions in associated
  namespaces, including those of class template type arguments in ordinary calls. Functions
  support default arguments, recursion, operator overloading and calls through pointers.
  Free function templates can instantiate omitted dependent scalar defaults after overload selection.
  Ordinary free binary operators use argument-dependent lookup, including associated base namespaces.
  Supported dependent binary operators on ordinary non-template classes also find associated
  nonmember candidates at instantiation. Ordinary unary minus can find hidden friends by
  argument-dependent lookup.
  Nonmember operators can be called explicitly by name, including with namespace qualification.
  [#219](https://github.com/yrashk/ycxx/issues/219)
  Overloaded free functions can take braced arguments for arithmetic parameters
  and ordinary aggregates with arithmetic fields. A braced argument to a known function can
  initialize a class temporary for a reference parameter, including `const std::initializer_list<int>&`
  with libycxx. A pointer or reference target type can select a free or static member overload.
  Ordinary non-template member defaults can refer to members declared later in the class.
  Namespace function definitions can inherit C language linkage from a matching earlier
  declaration, including definitions written inside their namespace and qualified definitions
  outside it. [#426](https://github.com/yrashk/ycxx/issues/426),
  [#455](https://github.com/yrashk/ycxx/issues/455)
  A single `extern "C"` or `extern "C++"` declaration gives an ordinary namespace `const`
  variable the linkage and definition rules of a written `extern` storage specifier.
  [#942](https://github.com/yrashk/ycxx/issues/942)
- ordinary scalar `new` and single-object `delete` for integral and pointer objects, including
  cv-qualified objects. Scalar allocation uses the global Itanium allocation functions.
  Syntax checking also covers one-dimensional scalar `new T[n]` and `delete[] p`.
  Unary `noexcept` accounts for the selected global array allocation function.
  On Linux x86-64, ordinary functions with cleanup-free unwind frames can default-initialize
  one-dimensional integral and ordinary pointer arrays with a bound built from integer literals,
  parentheses, unary `+` and `-`, and binary `+`, `-`, and `*`. The allocation returns the
  original address. `delete[] p` releases it through the unsized global deallocator, including
  for zero-length arrays.
  The usual aligned scalar allocation and deallocation signatures support explicit calls with
  `std::align_val_t`. Written nonthrowing global scalar and array overloads with a final
  `const Tag&` support explicit calls, including `std::nothrow_t` and user-defined class tags.
  The usual global `operator new[]` and `operator delete[]` signatures
  support explicit calls and typed address selection.
  The four written C++26 non-allocating placement functions support explicit calls, including
  in constant expressions.
  Runtime placement `new` supports integral and ordinary pointer objects with one placement
  argument and a visible written reserved allocation function.
  [#293](https://github.com/yrashk/ycxx/issues/293) · [#854](https://github.com/yrashk/ycxx/issues/854)
- pointers and references to arrays of unknown bound, and template partial specializations
  that match unknown-bound array types. [#222](https://github.com/yrashk/ycxx/issues/222)
- implicit lvalue copy assignment for ordinary classes without bases, including class members
  with their own assignment operators. [#360](https://github.com/yrashk/ycxx/issues/360)
- first-declaration in-class defaulted copy assignment for ordinary classes without bases.
  The written operator keeps its access and assigns supported members in declaration order.
  [#244](https://github.com/yrashk/ycxx/issues/244)
- user-declared copy and move assignment govern the availability of implicit constructors.
  [#244](https://github.com/yrashk/ycxx/issues/244)
- ordinary namespace-scope classes without bases can implicitly move from an rvalue when their
  implicit copy is deleted. This covers scalar fields and class fields with proven nonthrowing
  initialization and destruction.
  [#747](https://github.com/yrashk/ycxx/issues/747)
- ordinary named unions with a written destructor can hold nontrivially destructible members.
  The member's copy constructor can be trivial or deleted. An explicit union constructor
  initializes its selected member. A nonthrowing constructor works on both targets, and the
  hosted Linux subset above can unwind the construction. [#203](https://github.com/yrashk/ycxx/issues/203) ·
  [#738](https://github.com/yrashk/ycxx/issues/738)
- namespace-scope union templates support the same rules for concrete nontrivially destructible
  variants after instantiation. [#203](https://github.com/yrashk/ycxx/issues/203)
- classes and unions: data and static members, named data members of unnamed union type,
  member functions, constructors including ordinary delegation, and destructors
  with RAII, aggregate and designated initialization, namespace-scope objects initialized by
  code before `main` and destroyed after it, block-scope `static` variables initialized once, passing and returning them by value with
  Clang's calling convention, so ycxx and Clang objects call each other; access control
  (`public`, `protected`, `private`) and friend functions and classes. Ordinary unqualified
  friend definitions in non-template namespace-scope classes are found by argument-dependent
  lookup or through a matching namespace declaration. [#259](https://github.com/yrashk/ycxx/issues/259)
- Linux x86-64 can emit the base constructor and destructor entries for an ordinary class
  with scalar fields, no bases, and written out-of-line definitions. A separately compiled
  GCC or Clang caller can construct and destroy the ycxx-defined base as a subobject.
  [#136](https://github.com/yrashk/ycxx/issues/136)
- named `int` and `unsigned int` bit-fields of width 1 through 32 in ordinary unpacked,
  non-template classes without bases;
- single and multiple inheritance (not virtual): base classes laid out as Clang does, member
  lookup through bases, derived-to-base conversions and base initializers. Unqualified
  injected class names in ordinary member bodies respect base access.
- namespaces, nested (`namespace a::b`) and inline ones, typedefs and aliases, including
  function type and function pointer aliases with qualified return types, `using` declarations
  and directives;
- named function-pointer declarations with qualified return types, qualified names in grouped
  declarators, and unnamed function-pointer parameters;
- parenthesized direct initialization from functional conversions such as `S b(S(1));` and
  `Holder h(Tag(n), 2);`, including an additive expression inside a grouped operand as in
  `Holder h(Tag(n), Tag(n + 1));`. [#769](https://github.com/yrashk/ycxx/issues/769) · [#813](https://github.com/yrashk/ycxx/issues/813)
- `[[…]]` attributes, the GNU attributes and `asm` labels the macOS SDK's headers use, the
  GNU pointer qualifiers glibc uses, and the type-trait built-ins libycxx's headers use
  (`__is_same`, `__remove_cv`, …). `__is_destructible`, `__is_nothrow_destructible`, and
  `__is_trivially_destructible` give true for references, non-enum scalars, and bounded arrays
  of those scalars. They give false for cv `void`, function types, and arrays of unknown bound.
  [docs/EXTENSIONS.md](docs/EXTENSIONS.md)
- templates: function, class and variable templates, deduction, partial ordering, explicit and
  partial specializations with substitution checks for dependent type arguments, static constexpr
  members of class templates, member function and
  constructor templates, parameter packs and fold expressions, alias templates, template
  template parameters, concepts and requires-expressions (including `{ e } noexcept;`) in overload
  resolution and in choosing partial specializations, including
  explicit trailing type packs in ordinary constrained function overloads. Directly named
  non-dependent primary class specializations with explicit type arguments check constraints.
  Class templates and class partial specializations accept parenthesized requires-clause
  operands joined by `&&` or `||`.
  Non-template code outside requires-expression bodies also checks constraints on directly named
  primary namespace variable templates with explicit non-dependent type arguments. Templates support
  `if constexpr`, deferred template static assertion failures, and two-phase lookup, with
  Itanium names identical to Clang's. Native function templates can call an unambiguous
  externally linked target supplied as a function pointer argument, including a concrete
  `noexcept` signature. The compiler supports Clang's `__make_integer_seq`. Template type
  arguments can use terminal namespace-qualified alias specializations that expand to
  dependent member types. [#406](https://github.com/yrashk/ycxx/issues/406) · [#630](https://github.com/yrashk/ycxx/issues/630)
  Dependent calls through class and alias template qualifiers expand argument packs, including
  packs preceded by an ordinary template parameter. [#890](https://github.com/yrashk/ycxx/issues/890)
- dependent `__is_same`, `__is_reference`, `__is_const`, `__is_volatile`, `__is_array`,
  `__is_pointer`, `__is_function`, `__is_enum`, `__is_union`, `__is_class`, `__is_object`, and the
  three member-pointer predicates as `bool` template arguments or defaults, including through
  alias templates
  [#209](https://github.com/yrashk/ycxx/issues/209);
- dependent non-type template arguments from unqualified parameterless `consteval` function
  templates with explicit type arguments, and unqualified calls to a single ordinary external
  `consteval` function with direct integral template-parameter operands.
  [#209](https://github.com/yrashk/ycxx/issues/209)
- `__is_constructible`, `__is_nothrow_constructible` and `__is_trivially_constructible` for
  supported scalar, reference, function and `void` queries, including argument packs in
  class-template base and variable-template initializer replay.
  [Extension guide](docs/EXTENSIONS.md) · [#218](https://github.com/yrashk/ycxx/issues/218)
- `__is_assignable`, `__is_nothrow_assignable` and `__is_trivially_assignable` for arithmetic
  operands, including dependent operands in supported class-template bases and variable-template
  initializers. [Extension guide](docs/EXTENSIONS.md) · [#218](https://github.com/yrashk/ycxx/issues/218)
- `__is_convertible` and `__is_nothrow_convertible` for arithmetic values, arithmetic references
  and cv `void`, including dependent operands in the same template contexts.
  [Extension guide](docs/EXTENSIONS.md) · [#218](https://github.com/yrashk/ycxx/issues/218)
- `__reference_converts_from_temporary` and `__reference_constructs_from_temporary` for arithmetic
  and reference-related class operands. [#218](https://github.com/yrashk/ycxx/issues/218)
- `__is_base_of` for known class hierarchies, including private and ambiguous bases and dependent
  operands in supported class-template bases, variable-template initializers and function template
  bodies.
  [Extension guide](docs/EXTENSIONS.md) · [#218](https://github.com/yrashk/ycxx/issues/218)
- `__is_empty` accounts for inherited data members and supports dependent operands in class-template
  bases and variable-template initializers.
  [Extension guide](docs/EXTENSIONS.md) · [#218](https://github.com/yrashk/ycxx/issues/218)
- unqualified dependent `bool` variable template-ids as `bool` template arguments, and
  logical negation of a qualified dependent `bool` variable template-id in a `bool` template
  argument or default
  [#209](https://github.com/yrashk/ycxx/issues/209);
- a single nonvirtual, nonpolymorphic base in a primary class template, including an
  alias-selected dependent base and qualified lookup of its static members.
  [#308](https://github.com/yrashk/ycxx/issues/308)
- placeholder types: `auto` and `decltype(auto)` variables (`const auto&`, `auto*`, `auto&&`),
  deduced and trailing return types, in templates too. Ordinary block variables with an
  `= expression` initializer can use `Concept auto` with a prior unqualified type concept.
  [#286](https://github.com/yrashk/ycxx/issues/286)
- lambdas in function bodies: captures by copy and by reference, capture-defaults, `this` and
  `*this`, init-captures, `mutable`, conversion to a pointer to function, generic lambdas (`auto`
  parameters) and template lambdas (`[]<class T>`), and closures passed to and called from
  function templates, with Clang's names for closure types;
- noncapturing, non-generic lambdas as initializers of internal-linkage namespace variables,
  with addressable static storage and scalar calls ([#338](https://github.com/yrashk/ycxx/issues/338));
- casts, including `static_cast<T&&>` from a compatible object lvalue and runtime `static_cast`
  downcasts of object pointers, lvalue references and base xvalues through accessible,
  unambiguous nonvirtual base paths, `sizeof`/`alignof` (classes included), direct-member `offsetof` for
  ordinary standard-layout classes, `decltype`, and calls through pointers to functions.
  Qualified functional casts check access to aliases and typedefs declared directly in an
  ordinary class. [#199](https://github.com/yrashk/ycxx/issues/199) ·
  [#216](https://github.com/yrashk/ycxx/issues/216) · [#248](https://github.com/yrashk/ycxx/issues/248)
- equality and inequality between `std::nullptr_t` values and between a `std::nullptr_t`
  value and a pointer. [#199](https://github.com/yrashk/ycxx/issues/199)
- constant evaluation on ycxx's own interpreter: `constexpr`, `consteval`, `constinit`,
  `static_assert` and `if consteval`, with errors that explain why something is not constant.
  Scalar `&&`, `||` and `?:` expressions keep scalar reference-argument temporaries alive
  until the end of the full-expression on each executed path. [#902](https://github.com/yrashk/ycxx/issues/902)
  A constant evaluation can bypass a block-scope static aggregate. [#872](https://github.com/yrashk/ycxx/issues/872)
  Scalar enumeration values work in constexpr variables, calls, casts and switch conditions.
  A flat static constexpr aggregate member initialized with `T{...}` has addressable storage
  and runtime field access. Direct-list braces also work for aggregates of public integral
  fields in namespace-scope classes without bases.
  [#262](https://github.com/yrashk/ycxx/issues/262)
  Constant expressions can read non-mutable integral and enumeration fields of flat
  namespace-scope constexpr aggregates.
  [#862](https://github.com/yrashk/ycxx/issues/862) · [#876](https://github.com/yrashk/ycxx/issues/876)
  Namespace-scope `constexpr` pointers to `void` can retain scalar and array element addresses.
  `__builtin_addressof`, used by libycxx's `std::addressof`, bypasses overloaded `operator&`.
  It supports runtime addresses and constant expressions such as a scalar global's address.
  `__builtin_strlen`, used by libycxx's exception headers, scans character storage in constant
  expressions and native code. [#531](https://github.com/yrashk/ycxx/issues/531)
- `extern "C"` functions from libSystem, including `printf` with integers, characters,
  floating values, pointers and strings as arguments, and the SDK `<math.h>`'s built-ins
  (`__builtin_fabs`, `__builtin_inf`, `__builtin_huge_val`, `__builtin_nan`).

**Preprocessing.** All of translation phase 4: includes, object-like and function-like
macros, conditionals, `_Pragma` and the predefined macros. `ycxx cxx -E` matches Clang's output.
`ycxx cxx -E --target=x86_64-linux-gnu` selects Linux macros and resource headers. On native
x86-64 Linux it also searches glibc's multiarch include path and preprocesses glibc's `<stdio.h>`.
`<stdarg.h>` supplies `va_list` and its macros on both targets. On Linux, direct `<float.h>` and
`<limits.h>` provide the target's numeric limits. On native Linux, glibc's `<stdint.h>` and the pinned libycxx's
hosted `<cstdint>`, `<cinttypes>`, and `<inttypes.h>` compile. A Darwin target can use the configured
SDK or one selected by `SDKROOT`. Cross-target builds can also supply target headers with `-I` or
`-isystem`. When no target `<stdint.h>` is available, `<cstdint>` uses compiler predefines.
[#475](https://github.com/yrashk/ycxx/issues/475) · [#657](https://github.com/yrashk/ycxx/issues/657) ·
[#664](https://github.com/yrashk/ycxx/issues/664) · [#690](https://github.com/yrashk/ycxx/issues/690)

**Incremental builds through a resident daemon.** `ycxx cxx` hands its work to a per-project
`ycxx daemon` (`YCXX_NO_DAEMON=1` runs in-process), which watches the files it has read and keeps
its analysis between commands. A repeated build re-runs nothing, and an edit re-runs only what
it affects, through code generation and the link. The daemon serves concurrent requests on a
thread pool, and its output is byte for byte that of a cold, in-process build. The daemon, its file
watcher (FSEvents on macOS, inotify on Linux), the engine trace and the LSP server below work the
same on both hosts. [#659](https://github.com/yrashk/ycxx/issues/659)
Measured on 2026-11-22 (Release ycxx, arm64 macOS, 18 cores) with a generated 1 MB file of 290
modules and 11,311 functions (`playground/genbig.py`): a cold build takes 0.91 s, and changing
one module's constant rebuilds in 0.80 s. The back end re-runs only the two functions that read
the constant; the front end still re-analyses the whole file on any edit, about 0.5 s of that
([#938](https://github.com/yrashk/ycxx/issues/938), [#947](https://github.com/yrashk/ycxx/issues/947)).

**See what the engine does.** `ycxx cxx --trace-engine a.cpp` (or `YCXX_TRACE_ENGINE=stream`)
prints every query the build went through as a colored tree on stderr: what ran, what was
reused, verified green or cut off early, how long it took and on which worker, and how much
memory each cell allocated and retains, with sums per subtree, per query kind and per build. A second build
shows everything reused. A compile error is marked red at the cell where it starts, and its
message is shown up to the root. Diagnostic details include the file, line and column where
available. `--trace-engine=interactive` opens a browser of the trace after the compile: a tree
with each function's IR and assembly, a timeline of the pool's workers, and filters. It follows
the terminal's size and takes the mouse: click to select, fold and sort by a column, use the
wheel to scroll, and drag to resize the pane. It copies a cell, a subtree, selected rows or any
selected text of the IR and instructions to the clipboard, over SSH too. Its histogram view charts
self and total time, memory, runs, cache hits and waits by query kind, source file or child, and a
bar opens its cells; cells show and filter by their source position
([docs/TRACE.md](docs/TRACE.md#the-interactive-browser)).
`--trace-engine=json` writes the whole trace as JSON Lines, for tools and agents. Cells that
come from source (a file, a unit, a function's IR and code) carry their file and lines.
`ycxx cxx --watch` stays attached and builds again whenever a file the build read changes,
headers included; with `--trace-engine` each rebuild shows what it re-ran, and
`--trace-engine=interactive` keeps the browser open and adds each rebuild live. `ycxx ctl trace
[--interactive]` attaches to the daemon and shows every request it serves, and `ycxx ctl status`
reports its memory, with what each query kind retains.

**Editor support.** `ycxx lsp` is a language server that reports ycxx's errors as you type,
including those in the headers a file includes, and goes to the definition of a name used in a
function body, through the daemon, without unsaved text ever reaching a build. Each file is
checked with the flags of its last build, or else with its line in the project's
`compile_commands.json`.

Syntax checking enforces ordinary destructor access for automatic class objects and
explicit construction expressions. [#259](https://github.com/yrashk/ycxx/issues/259)

**Pointer-to-member declarations.** Data and function member-pointer types can appear in
declarations, type traits and requires-expressions. Syntax checking covers ordinary data member
addresses, typed nulls and same-class `.*` and `->*`. [#238](https://github.com/yrashk/ycxx/issues/238) · [#411](https://github.com/yrashk/ycxx/issues/411)

Ordinary non-template member functions in namespace-scope classes can use `&` and `&&`
to distinguish lvalue and rvalue receivers. [#311](https://github.com/yrashk/ycxx/issues/311)

**Virtual declarations.** Syntax checking accepts virtual members and inline definitions in
ordinary root classes. Ordinary pure virtual declarations define abstract interfaces through a
single nonvirtual base at each level. Ordinary virtual members can use `final` to prevent
further overrides. Ordinary named namespace-scope classes and structs can use class-head `final`
to prevent derivation. Derived classes also support unqualified base initializers with parentheses
or flat braces. On Linux x86-64 and arm64 macOS,
pointer and reference consumers can call virtual functions with fixed integer signatures on
peer-owned objects with an ordinary root, including a derived class with that single public
base, at most one integral field and at most one override. Consumers can upcast those derived
views, access ordinary integral members, and query `sizeof` and `alignof` for the Itanium layout.
A translation unit can also define one non-key virtual member of an eligible root out of class.
For that case, the peer provides the key function, virtual table and RTTI. A caller can
also dispatch through a peer-owned abstract interface with one or two
public pure virtual slots using fixed integer signatures and bare `noexcept`.
[#309](https://github.com/yrashk/ycxx/issues/309)

**Linux borrowed runtime casts.** `dynamic_cast<cv void*>(p)` accepts a pointer to
an admitted peer-owned polymorphic root. The cast preserves the pointee's qualifiers, returns
null for a null operand, and recovers the most-derived address from a live subobject.
On Linux x86-64, `dynamic_cast<D*>(p)` also supports a fieldless root `B` and its fieldless
derived class `D` with one public nonvirtual base `B`. Each class declares one public virtual
`int` member with no arguments, and `D` overrides that member. The hosted C++ peer defines
both key functions and links the consumer with their RTTI and the cast runtime.
The cast also supports a keyless `B` with one public pure `int f()` and a `D` with its public
override and one public `int` field. The consumer emits weak base RTTI, and the hosted peer
supplies the derived key, its RTTI, and the cast runtime.
[#294](https://github.com/yrashk/ycxx/issues/294) · [#930](https://github.com/yrashk/ycxx/issues/930)

**Structured bindings.** Local bindings can copy or alias scalar-element arrays, including
multidimensional arrays. They can also copy or alias ordinary non-template classes without
bases and bind to their public, addressable data members, including array and reference
members. [#285](https://github.com/yrashk/ycxx/issues/285)

**Virtual tables.** On Linux x86-64 and arm64 macOS, a translation unit with the key definition emits the vtable and
RTTI for an ordinary root with one public integral field and two fixed integer virtual
signatures. A peer supplies the second virtual. Both targets also support ordinary
automatic default initialization (`Root root;`) for that shape with an implicit default
constructor, a trivial destructor and no field initializer. The local object's vptr uses the
same table whether the key definition is in this translation unit or a peer. A plain automatic
derived object can also use a table from the same unit or a peer when its public abstract base
has one pure `int f()` and the derived class has one public `int` field and one override. The
unit defining the override out of class emits the derived table and both RTTI descriptors. The
caller can bind the base reference and expose it to the peer. On macOS the base class's RTTI
follows Apple's arm64 convention for non-unique type names.
[#309](https://github.com/yrashk/ycxx/issues/309) · [#650](https://github.com/yrashk/ycxx/issues/650)

Both targets emit all three virtual destructor entries for a fieldless, baseless root when
a peer defines its earlier key function. Complete and base destruction restore the root's
vptr before calling the nonthrowing observer. The deleting entry then uses global sized
`operator delete`. [#136](https://github.com/yrashk/ycxx/issues/136)

### Not yet

- The standard library and its headers. On macOS, C headers such as `<stdio.h>` compile without
  warnings and `printf` runs. libycxx's `<cstdio>` is still tracked under [#133](https://github.com/yrashk/ycxx/issues/133).
  `<cmath>` (which `<math.h>` includes) does not compile yet. [#349](https://github.com/yrashk/ycxx/issues/349)
- Native code generation for member-pointer values and general member-pointer expressions.
  [#411](https://github.com/yrashk/ycxx/issues/411)
- Constexpr lambdas (the interpreter has no class objects yet), further lambda declaration
  contexts ([#338](https://github.com/yrashk/ycxx/issues/338)),
  tuple-like structured bindings and structured bindings to bit-fields or inherited members,
  inheriting constructors, and base packs or multiple bases in a class template.
- Broader native construction, virtual dispatch and table emission, virtual base classes,
  `typeid`, other `dynamic_cast` forms, and virtual destruction for other class shapes.
  [#309](https://github.com/yrashk/ycxx/issues/309) · [#310](https://github.com/yrashk/ycxx/issues/310) ·
  [#294](https://github.com/yrashk/ycxx/issues/294) · [#136](https://github.com/yrashk/ycxx/issues/136)
- Member class and alias templates, conversion function templates, and constraints on class
  templates outside the directly named primary case and constraints on members.
- Native throwing beyond the Linux `int` slice, typed and multiple handlers, cleanup during
  source-level unwinding, and unwind discovery in ycxx-linked programs.
  [#89](https://github.com/yrashk/ycxx/issues/89) · [#558](https://github.com/yrashk/ycxx/issues/558)
- Optimization (`-O`) and the extended floating-point types (`std::float16_t`, …).
- Automatic Linux CRT and library selection and general-purpose ELF object interoperability.
  [#390](https://github.com/yrashk/ycxx/issues/390)
- Namespace-scope destruction for x86-64 Linux. [#400](https://github.com/yrashk/ycxx/issues/400)
- Parity between the two native back ends. x86-64 Linux does not yet pass classes with
  floating members by value, trivially copyable classes over 16 bytes as arguments, or x87
  `long double`. AArch64 macOS does not yet have native `int` throws, unwinding through ycxx
  frames or native array `new`, which x86-64 has. The
  full table is [#639](https://github.com/yrashk/ycxx/issues/639).

### In progress

- Standard headers, starting with `<cstdio>` ([#133](https://github.com/yrashk/ycxx/issues/133)).
- ι: statements and declarations, the C++98/11 core: default arguments, block-scope statics,
  casts and arrays ([#337](https://github.com/yrashk/ycxx/issues/337)).
- `auto` and lambdas ([#278](https://github.com/yrashk/ycxx/issues/278)): placeholder types,
  lambdas and generic lambdas are done; constexpr lambdas wait on class objects in the
  interpreter ([#262](https://github.com/yrashk/ycxx/issues/262)).

- x86-64 ↔ AArch64 back-end parity ([#639](https://github.com/yrashk/ycxx/issues/639)), one
  gap per PR in priority order.

Next come exceptions ([#89](https://github.com/yrashk/ycxx/issues/89)).

## Goals

- **The C++26 working draft as the specification.** Core language ([lex] through [except]),
  preprocessing ([cpp]) and the resolutions of the Core Working Group's issues list.
- **Resident and incremental.** A query-based engine with recorded dependencies, early cutoff
  and futures: an edit costs work proportional to what it changes, not to the size of the
  translation units that include it. Template instantiations, parsed headers and module
  interfaces are shared across translation units.
- **Highly parallel, still deterministic.** Output (objects, diagnostics, their order) is
  bit-identical regardless of thread count or scheduling.
- **The whole loop in one program.** No external assembler, linker or archiver. Targets in order:
  AArch64 Mach-O (macOS), x86-64 ELF (Linux), then AArch64 ELF.
- **Optimization by search, measured against the competition.** Optimized forms are memoized
  and improved while the compiler is idle. The optimizer uses equality saturation, solver-backed
  instruction selection, register allocation and scheduling, and cost models measured on real
  hardware. Every result is verified before use, and builds stay reproducible. A benchmark corpus
  tracks ycxx against Clang and GCC on every commit.
- **Itanium C++ ABI compatible.** Objects link and interoperate with those of Clang and GCC.
- **A drop-in `c++` front.** A thin client accepts the usual command-line interface and forwards
  to the resident compiler, so existing build systems and test suites can drive it. An LSP server
  sits on the same engine.

## Relationship to libycxx

ycxx is written in C++26 against [libycxx](https://github.com/yrashk/libycxx), an independent
C++26 standard library built the same way. libycxx is a git submodule at
[`deps/libycxx`](deps/libycxx). It is also the standard library ycxx ships for the programs it
compiles. Until ycxx can compile itself, it is built with GCC 16 (with reflection and
contracts enabled) and libycxx, and uses as much of C++20–26 as that compiler allows.

## How it is written

ycxx is implemented by AI agents (Claude, directed by the author) from the C++ working draft,
the CWG issues list, WG21 papers, the Itanium C++ ABI, the target architectures' and object
formats' published specifications (Arm and Intel architecture manuals, AAPCS64, the System V
psABI, ELF, Mach-O, DWARF) and cppreference.com. During development the agents have no access to
the sources of other C++ compilers, assemblers or linkers (Clang/LLVM, GCC/binutils, MSVC, EDG,
lld, mold, ld64, Zig's compiler) and do not read their generated code. Compilers are used as
black-box oracles: their outputs and accept/reject behaviour are compared with ycxx's. Their
conformance test suites may be run unmodified as external oracles, and their *test* files read
to triage failures. Nothing from them is copied into this repository.

This is not a clean-room claim in the legal sense. The models the agents run on were trained on
public code that very likely includes those compilers, so no separation from them can be shown,
only that no implementation source was consulted while ycxx was written. A similarity analysis
in the style of libycxx's (`deps/libycxx/docs/similarity/METHOD.md`) is planned. See
[DECISIONS.md §2](DECISIONS.md#2-provenance).

## License

ycxx is licensed under the **Apache License 2.0 with LLVM Exceptions**
(`Apache-2.0 WITH LLVM-exception`). See [LICENSE](LICENSE).
