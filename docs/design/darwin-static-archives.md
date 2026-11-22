# Darwin static archives

[#842](https://github.com/yrashk/ycxx/issues/842) adds explicit `.a` operands to arm64 macOS
executable links, the counterpart of the [Linux archive inputs](linux-static-archives.md) (#388).
libycxx's runtime (`libycxx.a`, `libycxx-abi.a`) and GCC's `libemutls_w.a` are Darwin archives,
and a ycxx program has to link them.

## Selection

The rules are the system linker's, observed as a black box (DECISIONS §2.3) where they differ
from the ELF rules:

- An archive serves names referenced anywhere on the command line, not only by earlier
  operands: `cc lib.a main.o` links on macOS.
- A weak reference selects a member, and so does a name defined only tentatively (a common
  symbol, from `-fcommon` C). The member's definition then replaces the tentative one.
- When several archives list a name, the first in operand order supplies it, and within an
  archive the first member that defines it.
- A definition in an ordinary object wins over every archive, wherever the object comes.

So every ordinary object is included first, in operand order. Each outstanding name, in the
order it became outstanding, then selects its member and the member's own references join the
queue. The selected members follow the ordinary objects in selection order. Names still
outstanding go to the dylibs afterwards (libSystem, then libc++abi), so an archive member wins
over libSystem as it does for the system linker, which searches libSystem last.

A member is decoded only when selected, and must define the name that selected it. Container
and index errors are reported whether or not any member would be selected.

## Format

A Darwin archive is the BSD variant of `ar(5)`: the `!<arch>\n` signature and 60-byte headers
shared with the GNU format, names either in the header field or stored as the first `<n>` bytes
of the member after a `#1/<n>` field (NUL-padded), and a ranlib index as the first member,
named `__.SYMDEF` or `__.SYMDEF SORTED` (`struct ranlib`, 32-bit fields) or the `_64` forms
(`struct ranlib_64`), from Apple's published `<mach-o/ranlib.h>`. The index is the array's size
in bytes, the entries (a string-table offset and the offset of the member's header in the file),
the string table's size and the table, in the target's byte order (little-endian on arm64).

The reader checks the array and table sizes, every string offset and terminator, and that every
entry names an ordinary member's header. It rejects thin archives, GNU `/` indexes and `//` long
names, an index that is not the first member, and a nonempty archive without one. Archive
production for Darwin stays with `ycxx archive` (#389).
