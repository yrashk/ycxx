# Linux x86-64 dynamic executables

[#390](https://github.com/yrashk/ycxx/issues/390) starts with one native link path for an
executable importing functions from explicitly supplied shared objects. The executable uses the
target dynamic loader and ycxx's startup. This exercises the executable-side dynamic ABI before
the driver selects a target C runtime.

## Driver contract

```sh
ycxx cxx --target=x86_64-pc-linux-gnu -nostdlib --dynamic-linker="$target_loader" \
  main.cpp "$target_library" -o main
```

Each shared-library input is an explicit file. The interpreter is an explicit absolute target
path, as recorded in a target executable's `PT_INTERP`. The driver may cross-link without
opening the interpreter on the build host. Each input library must declare a nonempty,
slash-free `DT_SONAME`. The output records these names in `DT_NEEDED` in command-line order.
Reject duplicate SONAMEs, linker scripts, missing interpreter and library arguments, and
dynamic options outside the Linux x86-64 executable link. The installed SONAME must be
discoverable by the interpreter at run time. Test fixtures set `LD_LIBRARY_PATH` for the
launched process. The executable records the SONAMEs without the build-host library paths.

Read shared objects as data. The same interpreter setting and ordered library input metadata
must reach the in-process driver, the daemon and the `Link` query. A changed provider at the
same path must invalidate the relevant link input. Keep object-only static links and the Darwin
path working through their existing entry points.

## ELF inputs and output

An input library is little-endian ELF64 `ET_DYN` for `EM_X86_64`. Read its mapped dynamic
table and resolve virtual addresses through file-backed `PT_LOAD` ranges. `DT_HASH.nchain`
bounds `DT_SYMTAB`. Validate table entry widths, file and virtual address bounds, hash and
symbol indices, and strings within `DT_STRSZ`. Diagnose GNU-hash-only providers in the first
slice. Use externally visible, defined, unversioned `STB_GLOBAL`/`STT_FUNC` exports with default
visibility. The first slice uses self-contained providers and rejects their `DT_NEEDED` entries.
Versioned imports, IFUNC, TLS, imported data, transitive dependencies and callbacks through
executable exports remain part of the broader shared-library input work in #390.

Produce fixed-address `ET_EXEC`. Map the ELF and program headers. Place `PT_PHDR` and the
NUL-terminated `PT_INTERP` before the load headers, keeping `PT_LOAD` entries in ascending
virtual-address order. Use page-congruent, nonoverlapping R, RX and RW load regions and an
RW, non-executable `PT_GNU_STACK`. Place `PT_DYNAMIC` in a mapped region. The interpreter
consumes `DT_NEEDED`, `DT_STRTAB`, `DT_STRSZ`, `DT_SYMTAB`, `DT_SYMENT`, `DT_HASH`,
`DT_RELA`, `DT_RELASZ`, `DT_RELAENT` and the final `DT_NULL`. Table pointers and relocation
destinations are target virtual addresses. `DT_NEEDED` values are offsets into the string
table. The string and symbol tables begin with their null entries. The runtime lookup path
uses this metadata without a section table.

Resolve definitions from the executable's objects using the existing static rules. For an
unresolved symbol, accept a producer-subset `R_X86_64_PLT32` displacement in an executable
section at a direct call, with addend −4. Check a full four-byte displacement immediately
after the `E8` opcode within the section. This check covers the supported ycxx object subset.
Reject incompatible references to the same imported name, including address-taking through
an absolute or ordinary PC-relative fixup. The external function-address path has its own
issue [#407](https://github.com/yrashk/ycxx/issues/407).

Allocate one RX thunk and one aligned RW address slot per imported name. Resolve the call
with the signed 32-bit `L + A - P` displacement to its thunk. The thunk tail-jumps through
the slot with a checked RIP-relative displacement, preserving return address, argument
registers, stack arguments and `%al`. An `Elf64_Rela` entry with `R_X86_64_GLOB_DAT` and
zero addend makes the loader fill the slot eagerly. The imported dynamic symbol is
`SHN_UNDEF`/`STB_GLOBAL`/`STT_FUNC`/`STV_DEFAULT` with zero `st_value`. The linker checks
that a supplied library provides the name. The runtime loader applies its usual lookup and
interposition rules. All loader relocation targets are writable slots.

## Entry and exit

Use the AMD64 initial stack to recover `argc` and `argv` and preserve call alignment. At
dynamic entry save the loader termination callback in `%rdx` before executing initializers
or `main`. The loader initializes providers before transferring control. The executable
runs its own initializer table once in the existing order from #387 and calls `main`.
Keep the executable's initializer table private to this startup path so the loader and
startup cannot both dispatch it. Preserve the result across the nonnull loader callback,
invoked once before the raw exit system call. Reject custom `_start` in this first dynamic
driver path. Program destructor registration remains tracked by #400.

## Verification and hosted continuation

- On Linux x86-64, run native output that calls an independently built, libc-free leaf DSO.
  Check normal and stack-passed INTEGER arguments and a nonzero result. Move the DSO and
  rerun via a test-scoped search path. Use a second implementation with the same SONAME
  to confirm runtime binding. Exercise loader initialization and termination with observable
  fixture effects and verify executable initialization precedes `main` exactly once.
- Inspect ELF, program headers, dynamic tables and relocations with `readelf`. Run rejected
  input tests for bad bounds, unsupported providers, missing definitions, wrong relocation
  forms and incomplete options. Compare in-process and daemon outputs and retain the
  static Linux and Darwin regression checks.
- Continue #390 with target CRT objects, versioned imports, GNU-hash providers, normal
  library discovery and process exit through the C runtime. Validate redirected stdio
  flushing and constructor order through the hosted driver. Archive input #388 and native
  shared-library creation #391 have separate interoperability gates.

Astra independently evaluated the fixed-address ELF format, eager GOT thunks, interpreter
and SONAME policy, dynamic entry and termination, and the boundary with target CRT startup.
The design uses the [ELF gABI program headers](https://gabi.xinuos.com/elf/07-pheader.html),
[ELF gABI dynamic linking](https://gabi.xinuos.com/elf/08-dynamic.html) and
[System V AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/jobs/artifacts/master/raw/x86-64-ABI/abi.pdf?job=build)
§§3.4.1, 4.4, 5.2 and B.1.
