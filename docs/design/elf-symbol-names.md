# ELF symbol names

An ELF string table index identifies a byte, including a byte inside a longer name.
Different symbols can share an index, and a nonzero index can point to a NUL. Index zero
denotes no name, including when the symbol string table is empty. The ET_REL reader checks
the table bounds, its boundary NULs when nonempty, and every nonzero index.
[#484](https://github.com/yrashk/ycxx/issues/484) · [ELF gABI §4](https://gabi.xinuos.com/elf/04-strtab.html)

The normalized object uses a name value containing either generated text or an offset and
length in a shared immutable copy of the input symbol string table. COMDAT signatures use
the same backing as symbols. Copying or moving names keeps the table alive, including when
archive extraction or group selection relocates the records. Equality and fingerprints use
the referenced bytes. Borrow views only while their owners are stable, and retain name
values for the later archive index emission.

Sort the referenced nonzero offsets and find each referenced NUL-terminated run's end
once. For a table of T bytes and S symbols, reader storage is O(T + S) even when every
symbol names a different suffix of the same run. ELF object and GNU archive writers still
emit their ordinary string tables and enforce each format's output size limits.

Astra independently reviewed the ELF rules, storage bound, and name ownership for
symbols, COMDAT selection, and archive output.
