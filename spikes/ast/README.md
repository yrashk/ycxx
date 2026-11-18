# AST representation spike (throwaway)

A probe for [yrashk/ycxx#2](https://github.com/yrashk/ycxx/issues/2) and
[`docs/design/ast.md`](../../docs/design/ast.md). It settles the mechanism questions whose answer
depends on what GCC 16.2 `-freflection` can do, so the design does not argue them in prose. It is
not the start of the real AST and may be deleted once the design is accepted. It builds standalone,
with plain headers and libstdc++, because the build skeleton (#4) does not exist yet:

```bash
make check
```

## What it shows (GCC 16.2.0, macOS arm64)

1. **A node schema is a list of plain aggregates.** `kinds<IntLit, NameRef, Binary, Call, Return,
   Block>` is the closed list. Fingerprint, structural equality, hash-consing and child walking
   are written once, generically, by reflection (`nonstatic_data_members_of`,
   `define_static_array`, `template for`, splices). An annotation (`[[=no_fp]]`) excludes a field
   from fingerprint and equality.
2. **Hash-consing with Merkle fingerprints, independent of interning order.** A child edge is
   `{child, offset relative to the parent}`; the child holds no position. `x + 1` occurring twice
   in a body is one node. Two interners that intern the same tree in different orders give it
   different handles and equal fingerprints.
3. **A red view that is never stored.** A stack cursor (green handle, item-relative offset, parent
   cursor) computes the offset of each occurrence of a shared node.
4. **Type canonicalization.** `QualType` is a type-node index with cv in its low bits. Sugar
   (`typedef`) keeps the type it spells; `canonical()` looks through it and pushes cv into array
   elements ([basic.type.qualifier]). `const A` with `typedef int A[3]` is the same canonical
   handle as `const int[3]`, with equal fingerprints across tables built in different orders.
5. **Dispatch over a realistic kind count.** A function-pointer table generated from the kinds
   list dispatches in O(1): 6.5–6.9 ns per visit over 240 kinds, 4.4 ns over 6.
6. **Compile time.** 240 kinds, each with reflection-derived code and a dispatch entry, take about
   2 s of front-end time and 7.5 s for the whole `-O2` build. Reflection and constant evaluation are
   about 1% of it. The first version kept the kinds in a `std::tuple<std::vector<Ts>...>`, and that
   alone cost over 50 s, almost all of it overload resolution inside `std::get` on a 240-element
   tuple. Per-kind storage is therefore an array of untyped pools indexed by kind, never a tuple
   over the kinds list.

ASan and UBSan builds run clean (`build/spike-asan`, 24 kinds to keep it quick).
