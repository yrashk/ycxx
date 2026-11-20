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

**Pre-0.1** (as of 2026-11-20). ycxx compiles a substantial subset of C++ (C-like code, classes,
templates and constant evaluation) on macOS arm64, from source to a signed, running executable,
with no external compiler, assembler or linker. It also builds static executables from a
freestanding C++ subset for x86-64 Linux. The standard library is out of reach. The plan is in
[DECISIONS.md](DECISIONS.md), the details of the code in [STATUS.md](STATUS.md), and the work in
[GitHub issues](https://github.com/yrashk/ycxx/issues) and the
[roadmap](https://github.com/yrashk/ycxx/issues/12).

### What you can do today

**Compile and run programs on macOS.** `ycxx cxx a.cpp -o a` (also `-c`, `-fsyntax-only`, `-E`, `-I`,
`-D`, `-std=c++26`, and `-fclass-call-abi=apple` to pass classes as Apple Clang does) builds an executable with ycxx's own front end, IR, AArch64 code generator
and Mach-O linker. It also links object files from Clang or GCC. Errors are reported as
`file:line:col: error: …`.

```sh
tools/build
build/bin/ycxx cxx hello.cpp -o hello && ./hello
```

**Preprocess binary resources.** Both targets support `#embed` and `__has_embed` with the standard
parameters. [#125](https://github.com/yrashk/ycxx/issues/125)

**Compile for x86-64 Linux.** Select `--target=x86_64-pc-linux-gnu` to compile with the
System V AMD64 backend. Use `-c` to write an ELF object, or `-nostdlib` to link C++ sources,
ycxx-generated `.o` files, or a mixture into a static executable. The startup runs namespace-scope
initializers, then calls `int main()` or `int main(int, char**)` and passes its return value to Linux
as the exit status. A program without such initializers can supply its own `_start`. Given
`example.cpp` containing
`int main() { return 0; }`, run the following on x86-64 Linux.

```sh
build/bin/ycxx cxx --target=x86_64-pc-linux-gnu -c example.cpp
build/bin/ycxx cxx --target=x86_64-pc-linux-gnu -nostdlib example.o -o example
./example
```

Fixed-prototype direct calls to free functions support passing and returning nonempty flat
records of integral fields by value. The record can occupy up to 16 bytes and must be trivial
for calls. [Linux integer aggregate ABI](docs/design/linux-integer-aggregates.md)

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
arguments. Ordinary free variadic definitions with scalar INTEGER signatures support
`<cstdarg>` and can forward a System V `va_list` to a separately compiled consumer.
Use the host linker to link these objects with libc.
[#417](https://github.com/yrashk/ycxx/issues/417), [#412](https://github.com/yrashk/ycxx/issues/412),
[#399](https://github.com/yrashk/ycxx/issues/399), [#196](https://github.com/yrashk/ycxx/issues/196)

Direct calls on x86-64 Linux support those result types when deduced from `auto` or
`decltype(auto)`. [#441](https://github.com/yrashk/ycxx/issues/441)

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

On x86-64 Linux, direct calls and saved pointers across translation units share one address for
simple external inline functions. [#422](https://github.com/yrashk/ycxx/issues/422)

External inline namespace variables with relocation-free static initializers share one object
across x86-64 ELF translation units. This lets two freestanding units using libycxx `<cstddef>`
link with the pinned static library. [#432](https://github.com/yrashk/ycxx/issues/432)

ycxx ELF objects record function types and code sizes. The ELF linker accepts supported
system-assembled objects with function symbols and the GNU-built libycxx static linkage-marker
object. Symbol names can share suffixes in an input string table.
[#428](https://github.com/yrashk/ycxx/issues/428), [#419](https://github.com/yrashk/ycxx/issues/419),
[#484](https://github.com/yrashk/ycxx/issues/484)

Both targets support cooked integer user-defined literals through namespace-scope literal
operators with an `unsigned long long` parameter, including constant evaluation.
[#289](https://github.com/yrashk/ycxx/issues/289)

Both targets support typed constant addresses and reads within named namespace-scope constexpr
arrays of supported scalar types, including arithmetic on saved pointers just past a nested row.
[#111](https://github.com/yrashk/ycxx/issues/111)

Both targets support `constexpr` constructors in class definitions and plain out-of-class
definitions for runtime construction. [#157](https://github.com/yrashk/ycxx/issues/157)

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
  Nonmember operators can be called explicitly by name, including with namespace qualification.
  [#219](https://github.com/yrashk/ycxx/issues/219)
  Overloaded free functions can take braced arguments for arithmetic parameters
  and ordinary aggregates with arithmetic fields. A pointer or reference target type can select
  a free or static member overload.
  Ordinary non-template member defaults can refer to members declared later in the class.
  Namespace function definitions can inherit C language linkage from a matching earlier
  declaration, including definitions written inside their namespace and qualified definitions
  outside it. [#426](https://github.com/yrashk/ycxx/issues/426),
  [#455](https://github.com/yrashk/ycxx/issues/455)
- ordinary scalar `new` and single-object `delete` for integral and pointer objects, including
  cv-qualified objects. Allocating expressions use the global Itanium allocation functions.
  [#293](https://github.com/yrashk/ycxx/issues/293)
- pointers and references to arrays of unknown bound, and template partial specializations
  that match unknown-bound array types. [#222](https://github.com/yrashk/ycxx/issues/222)
- classes and unions: data and static members, named data members of unnamed union type,
  member functions, constructors and destructors
  with RAII, aggregate and designated initialization, namespace-scope objects initialized by
  code before `main` and destroyed after it, block-scope `static` variables initialized once, passing and returning them by value with
  Clang's calling convention, so ycxx and Clang objects call each other; access control
  (`public`, `protected`, `private`) and friend functions and classes. Ordinary unqualified
  friend definitions in non-template namespace-scope classes are found by argument-dependent
  lookup or through a matching namespace declaration. [#259](https://github.com/yrashk/ycxx/issues/259)
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
- parenthesized direct initialization from functional conversions such as `S b(S(1));`;
- `[[…]]` attributes, the GNU attributes and `asm` labels the macOS SDK's headers use, the
  GNU pointer qualifiers glibc uses, and the type-trait built-ins libycxx's headers use
  (`__is_same`, `__remove_cv`, …)
  ([docs/EXTENSIONS.md](docs/EXTENSIONS.md));
- templates: function, class and variable templates, deduction, partial ordering, explicit and
  partial specializations with substitution checks for dependent type arguments, static constexpr
  members of class templates, member function and
  constructor templates, parameter packs and fold expressions, alias templates, template
  template parameters, concepts and requires-expressions in overload resolution,
  `if constexpr`, deferred template static assertion failures, and two-phase lookup, with
  Itanium names identical to Clang's; Clang's
  `__make_integer_seq`;
- placeholder types: `auto` and `decltype(auto)` variables (`const auto&`, `auto*`, `auto&&`),
  deduced and trailing return types, in templates too;
- lambdas in function bodies: captures by copy and by reference, capture-defaults, `this` and
  `*this`, init-captures, `mutable`, conversion to a pointer to function, generic lambdas (`auto`
  parameters) and template lambdas (`[]<class T>`), and closures passed to and called from
  function templates, with Clang's names for closure types;
- noncapturing, non-generic lambdas as initializers of internal-linkage namespace variables,
  with addressable static storage and scalar calls ([#338](https://github.com/yrashk/ycxx/issues/338));
- casts, including `static_cast<T&&>` from a compatible object lvalue and runtime `static_cast`
  downcasts of object pointers and lvalue references through accessible, unambiguous
  nonvirtual base paths, `sizeof`/`alignof` (classes included), `decltype`, and calls
  through pointers to functions. [#199](https://github.com/yrashk/ycxx/issues/199)
- constant evaluation on ycxx's own interpreter: `constexpr`, `consteval`, `constinit`,
  `static_assert` and `if consteval`, with errors that explain why something is not constant.
  Namespace-scope `constexpr` pointers to `void` can retain scalar and array element addresses.
- `extern "C"` functions from libSystem, including `printf` with integers, characters,
  floating values, pointers and strings as arguments, and the SDK `<math.h>`'s built-ins
  (`__builtin_fabs`, `__builtin_inf`, `__builtin_huge_val`, `__builtin_nan`).

**Preprocessing.** All of translation phase 4: includes, object-like and function-like
macros, conditionals, `_Pragma` and the predefined macros. `ycxx cxx -E` matches Clang's output.
`ycxx cxx -E --target=x86_64-linux-gnu` selects Linux macros and glibc's multiarch include path
and preprocesses glibc's `<stdio.h>`. Linux `<stdarg.h>` supplies the `va_list` declaration, and
direct `<float.h>` and `<limits.h>` provide the target's numeric limits.
[#475](https://github.com/yrashk/ycxx/issues/475)

**Incremental builds through a resident daemon.** `ycxx cxx` hands its work to a per-project
`ycxx daemon` (`YCXX_NO_DAEMON=1` runs in-process), which watches the files it has read and keeps
its analysis between commands. A repeated build re-runs nothing, and an edit re-runs only what
it affects, through code generation and the link. The daemon serves concurrent requests on a
thread pool, and its output is byte for byte that of a cold, in-process build.

**See what the engine does.** `ycxx cxx --trace-engine a.cpp` (or `YCXX_TRACE_ENGINE=stream`)
prints every query the build went through as a colored tree on stderr: what ran, what was
reused, verified green or cut off early, how long it took and on which worker, and how much
memory each cell allocated and retains, with sums per subtree, per query kind and per build. A second build
shows everything reused. A compile error is marked red at the cell where it starts, and its
message is shown up to the root. Diagnostic details include the file, line and column where
available. `--trace-engine=interactive` opens a browser of the trace after the compile: a tree
with each function's IR and assembly, a timeline of the pool's workers, and filters.
`--trace-engine=json` writes the whole trace as JSON Lines, for tools and agents.
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

**Pointer-to-member declarations.** Data and function member-pointer types can appear in
declarations, type traits and requires-expressions. [#238](https://github.com/yrashk/ycxx/issues/238)

**Array structured bindings.** Local bindings can copy scalar-element arrays or alias their
elements, including whole rows of multidimensional arrays. [#285](https://github.com/yrashk/ycxx/issues/285)

### Not yet

- The standard library and its headers. On macOS, C headers such as `<stdio.h>` compile without
  warnings and `printf` runs. libycxx's `<cstdio>` is still tracked under [#133](https://github.com/yrashk/ycxx/issues/133).
  `<cmath>` (which `<math.h>` includes) does not compile yet. [#349](https://github.com/yrashk/ycxx/issues/349)
- Taking member addresses, applying `.*` or `->*`, and code generation for member-pointer values.
  [#411](https://github.com/yrashk/ycxx/issues/411)
- Constexpr lambdas (the interpreter has no class objects yet), further lambda declaration
  contexts ([#338](https://github.com/yrashk/ycxx/issues/338)),
  member and tuple-like structured bindings, virtual functions, virtual base classes,
  inheriting constructors, and bases of class templates.
- Member class and alias templates, conversion function templates, and constraints on class
  templates and their members.
- Exceptions, optimization (`-O`), and the extended floating-point types (`std::float16_t`, …).
- Automatic Linux CRT and library selection and general-purpose ELF object interoperability.
  [#390](https://github.com/yrashk/ycxx/issues/390)
- Namespace-scope destruction for x86-64 Linux. [#400](https://github.com/yrashk/ycxx/issues/400)

### In progress

- Standard headers, starting with `<cstdio>` ([#133](https://github.com/yrashk/ycxx/issues/133)).
- ι: statements and declarations, the C++98/11 core: default arguments, block-scope statics,
  casts and arrays ([#337](https://github.com/yrashk/ycxx/issues/337)).
- `auto` and lambdas ([#278](https://github.com/yrashk/ycxx/issues/278)): placeholder types,
  lambdas and generic lambdas are done; constexpr lambdas wait on class objects in the
  interpreter ([#262](https://github.com/yrashk/ycxx/issues/262)).

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
