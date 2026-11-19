# Linux x86-64 static archive inputs

[#388](https://github.com/yrashk/ycxx/issues/388) starts with explicit `.a` operands in Linux
x86-64 static executable links. A system archiver can package the ELF relocatable objects
ycxx already accepts. The link query reads the archive and selects members in process.

## Input order and extraction

In a command with an archive, retain each source, disk object and archive in command-line
order. Source translation units can build concurrently. Include each ordinary object at
its operand and update the existing linker's definitions and outstanding nonweak references.
At an archive, use its index to select the earliest physical member defining any outstanding
symbol. Register the entire selected object with the same resolver and repeat until the
archive supplies no further members. A later occurrence of the same archive is another
search opportunity. Previously processed archive operands remain retired.

An undefined weak reference creates no archive demand. A weak or strong definition can
satisfy a nonweak reference. When another symbol causes extraction, the normal weak and
strong precedence and duplicate-definition checks apply to the complete member. Treat
global symbols with hidden visibility as definitions and references in the static link.
Use the selected objects for the existing startup, layout and relocation checks. Include
at least one source or ordinary object in the command. A link entry from an archive is
available when a member defining it is selected by a reference. Commands with only sources
and disk objects retain their existing ordering.

The first driver path uses `-nostdlib`. Diagnose an archive combined with explicit dynamic
link options. That combination needs a resolver which models dynamic providers at their
operand positions. Shared inputs remain covered by #390. Archive groups and other archive
search forms remain part of #388.

## Archive format

Accept the regular `!<arch>\n` global signature, packed 60-byte member headers with the
required trailer, bounded ASCII-decimal sizes and even member alignment. Recognize short
`name/` entries, the optional GNU `//` long-name table, and `/<decimal offset>` at a valid
terminated table entry. Treat member names as archive metadata. An index is a `/` member
with a big-endian 32-bit count, that many absolute member-header offsets and bounded
NUL-terminated symbol names. Check every index offset against an ordinary member header.
Support repeated names and retain physical member order. Accept an empty archive. Diagnose
a nonempty unindexed archive, thin archives, `/SYM64/` and BSD/Darwin index and inline-name
formats in this first path.

Validate container and index bounds before selection. Decode the ELF payload only when a
member is selected, using the existing relocatable-object reader. Confirm the member
defines the symbol that triggered extraction. Use `archive(member)` in selected-input
diagnostics. The usual undefined-symbol diagnostic still applies when the index provides
no eligible definition. Archive input cannot extend the relocatable-object subset.

## Driver and query

Snapshot archive bytes with the other file operands at driver ingress. Pass an ordered,
tagged manifest through the in-process and daemon paths. Include the archive content,
occurrence and target in the link input fingerprint so same-path replacement is visible.
The link query uses that snapshot and never extracts member files to disk. Reuse the
existing symbol registration and precedence rules when selecting and finally linking the
member objects.

## Verification

- Run native Linux x86-64 executables linked from user sources, separate ycxx objects and
  an archive produced by the system tool. Exercise transitive extraction, ordering across
  archives and objects, repeated operands, weak references and definitions, unused invalid
  members, and failure when a required member is invalid.
- Check malformed container and index bounds and the unsupported format diagnostics.
  Verify byte equality between in-process and daemon output and repeat after replacing an
  archive at the same path. Keep ordinary Linux and Darwin executable regressions green.
- Native archive creation #389 remains independent. Mach-O archive inputs, combined archive
  and DSO resolution, and additional system-library inputs remain under #388 and #390.

Astra independently reviewed extraction semantics, format bounds, daemon input ordering,
and the separation from shared-input resolution. The design uses [POSIX `c17` operand order](https://pubs.opengroup.org/onlinepubs/9799919799/utilities/c17.html),
[ELF gABI symbol binding](https://gabi.xinuos.com/elf/05-symtab.html#symbol-binding) and the
[SVR4/GNU archive format](https://man.freebsd.org/cgi/man.cgi?query=ar&sektion=5).
