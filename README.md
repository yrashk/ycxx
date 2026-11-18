# ycxx

**An independent, resident C++26 compiler.**

ycxx is being built to compile C++26 from source to linked executables by itself: preprocessor,
front end, constant evaluator, code generator, object writer and linker. It is designed not to
run once per translation unit and exit, but to stay resident, hold the whole program's analysis
in memory, and, when a file changes (reported by a file-system watcher, an editor or a build
tool), recompute only what that change can affect.

## Status

**Pre-0.1: the first end-to-end slice works** (updated 2026-11-18). The plan is in
[DECISIONS.md](DECISIONS.md), what is true of the code in [STATUS.md](STATUS.md), and the work
in [GitHub issues](https://github.com/yrashk/ycxx/issues) and the
[roadmap](https://github.com/yrashk/ycxx/issues/12).

**What works**, on macOS arm64 only:
- `ycxx cxx a.cpp -o a` compiles a small C-like subset of C++ to a signed arm64 Mach-O
  executable. ycxx's own lexer, parser and semantic analysis, L1 IR, AArch64 code generator and
  Mach-O linker do the work, with no external compiler, assembler or linker.
  - The subset: `int`, `long`, `unsigned`, `bool`, `char`, `void` and pointers; functions,
    recursion and calls; locals and namespace-scope variables with constant initializers;
    `if`, `while`, `do`, `for`, `break` and `continue`; the arithmetic, bitwise, shift,
    comparison, logical, conditional and assignment operators; string literals; and
    `extern "C"` calls into libSystem (`puts`, `putchar`, and `printf` with locals, arrays,
    globals and more arguments than registers, up to 32).
  - Structs and classes with data members (2026-11-18): `.` and `->`, pointers to and arrays of
    structs, aggregate initialization (`S s = {1, 2};`, `S s{…}`, nested braces, brace elision,
    designated initializers, `int a[] = {…}`), constant default member initializers, copies and
    assignment, and namespace-scope structs with constant initializers. Member functions (in and
    out of the class, `const`, static), `this`, and static data members. Constructors and
    destructors (RAII: destructors run at every block exit, `return`, `break` and `continue`),
    temporaries, references, and overloading by exact match, promotion and conversion; scoped and
    unscoped enumerations; namespaces (nested, inline and unnamed ones, qualified names,
    `using namespace`, `using N::x;` in a block, aliases, members defined outside their
    namespace). Not yet: passing or returning a class by value, inheritance.
  - Function and class templates (2026-11-18): templates at namespace scope with type and
    integral non-type parameters and default arguments, template argument deduction from calls
    (through class template specializations too), explicitly specified arguments
    (`f<int>(x)`), overload resolution among templates and functions with partial ordering,
    two-phase name lookup with argument-dependent lookup at the end of the translation unit, class
    templates with member functions in and outside the class, constructors and destructors, and
    instantiation on use, variable templates, explicit and partial specializations, with Itanium
    names that match Clang's. Not yet: member and alias templates, parameter packs, concepts, and
    a specialization used in two translation units of one program (its definitions are not coalesced yet,
    [#124](https://github.com/yrashk/ycxx/issues/124)).
  - Casts and type queries (2026-11-18): `static_cast`, functional and C-style casts between the
    supported types, `sizeof` and `alignof` of non-class types, `decltype`, and `std::nullptr_t`
    objects. `sizeof` of a class type is not yet supported
    ([#194](https://github.com/yrashk/ycxx/issues/194)); other cast gaps are
    [#199](https://github.com/yrashk/ycxx/issues/199).
  - What C and system headers declare (2026-11-18, toward `#include <cstdio>`,
    [#133](https://github.com/yrashk/ycxx/issues/133)): `typedef` and `using T = …`, including
    `typedef struct { … } T;`; `using N::x;` at namespace scope; unions (named, typedef'd and
    template unions; not yet anonymous ones or members with non-trivial special members,
    [#203](https://github.com/yrashk/ycxx/issues/203)); `float`, `double` and `long double` as
    types, though not yet their values ([#201](https://github.com/yrashk/ycxx/issues/201));
    `char s[] = "…"` and `extern T a[];`; `[[…]]` attributes, and the GNU `__attribute__((…))`
    and `__asm("…")` labels the macOS SDK uses ([docs/EXTENSIONS.md](docs/EXTENSIONS.md)). Errors
    inside a header are reported at the header's file and line.
  - Constant evaluation, by ycxx's own interpreter of its IR: `constexpr` and `consteval`
    functions and variables, `constinit`, `static_assert` and `if consteval`, with an error that
    says why an expression is not constant (overflow, an uninitialized read, a dangling pointer,
    a call to a non-constexpr function, …). Constant array bounds can come from `const` locals.
  - `-o`, `-c`, `-fsyntax-only` and `-std=c++26` are accepted. Errors are reported as
    `file:line:col: error: …`, with exit status 1.
- The preprocessor, in part (updated 2026-11-18):
  - `#include` of your own headers (`"..."`, `<...>`, `-I`, `-isystem`, `-iquote`,
    `#include_next`) and `#pragma once`;
  - object-like and function-like macros (`#define`, `#undef`, `-D`, `-U`), with `#`, `##`,
    `__VA_ARGS__` and `__VA_OPT__`;
  - `#if` and the other conditionals, `#line`, `#error`, `#warning`, `_Pragma`, and the
    predefined macros (`__ycxx__`, the target's, `__LINE__`, `__FILE__`, `__COUNTER__`, …);
  - `ycxx cxx -E` prints what Clang's `-E` does.
- A resident daemon, opt-in (2026-11-18): with `YCXX_DAEMON=1`, `ycxx cxx` sends its command
  line to a per-project `ycxx daemon`, starting one if needed. The daemon watches the files it
  has read (FSEvents on macOS, inotify on Linux) and keeps one engine session between commands:
  a repeated build re-runs nothing, and an edit re-runs only what read the changed file, through
  code generation and the link (checked against a cold build by the `.incr` tests). Its answers
  are byte for byte those of an in-process run, which is also the fallback on any failure.
  `ycxx ctl status` and `ycxx ctl shutdown` talk to it.
- A language server (2026-11-18): an editor that runs `ycxx lsp` gets ycxx's diagnostics for
  open files on open, change and save, through the daemon. Unsaved text, in the file or in a
  header it includes, reaches the diagnostics and never a build. A file is checked with the
  command line of the last `ycxx cxx` that built it through the daemon (with `YCXX_DAEMON=1`),
  else alone in its directory. Not yet: go-to-definition (#170) and `compile_commands.json`
  (#167).
- The acceptance tests in `tests/own/slice` run with ycxx as the compiler in CI, and every
  [draft example](tests/draft) is checked against Clang and GCC.

```sh
tools/build
build/bin/ycxx cxx hello.cpp -o hello && ./hello
```

**Not yet:**
- `#embed`, and the standard library's headers: `<cstdio>` now preprocesses, but libycxx does
  not yet know ycxx (yrashk/libycxx#17).
- Inheritance and virtual functions, and passing classes by value.
- `if constexpr`, and taking the address of a variable in a namespace-scope initializer
  (`int* p = &g;` passes the front end but not code generation yet).
- Exceptions.
- The standard library.
- Optimization (`-O`).
- The resident daemon as the default, with concurrent requests
  ([#137](https://github.com/yrashk/ycxx/issues/137),
  [#139](https://github.com/yrashk/ycxx/issues/139)). Without `YCXX_DAEMON=1`, `ycxx cxx` runs
  in-process, once per invocation, and the daemon serves one request at a time.
- Linux and x86-64.

Nothing beyond the subset above compiles, so neither real programs nor libycxx do yet.

**In progress**, one slice per epic:
- the resident daemon: concurrent requests, and making it the default
  ([#84](https://github.com/yrashk/ycxx/issues/84));
- classes and the object model ([#86](https://github.com/yrashk/ycxx/issues/86));
- templates and overload resolution: function, class and variable templates and their
  specializations work; parameter packs and concepts are next
  ([#88](https://github.com/yrashk/ycxx/issues/88));
- including the standard headers: libycxx's compiler detection
  ([yrashk/libycxx#17](https://github.com/yrashk/libycxx/issues/17)), then `<cstdio>`
  through sema ([#133](https://github.com/yrashk/ycxx/issues/133)).

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
