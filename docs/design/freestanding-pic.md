# Explicit PIC

The pinned libycxx freestanding archive requests `-fPIC` through CMake's
`POSITION_INDEPENDENT_CODE` property. The compiler driver and the editor share
the option. On x86-64 Linux `-fPIC` defines `__PIC__` and `__pic__` as `2` and
`-fpic` as `1`; the last of `-fpic`, `-fPIC` and `-fno-pic` wins. Both levels
emit the same object code. The effective setting is part of
the source identity and the serialized mode byte. Earlier mode values decode
with PIC disabled.

arm64 Darwin code is always position independent (`ast::Target::always_pic`).
As with Clang, both macros are always `2` there and the options change nothing
(#1089).

Astra reviewed reuse of the existing Linux object emission. Ordinary nonlocal
addresses use the GOT, direct calls use PLT-relative relocations, and local
addresses are RIP-relative. Static pointer initializers use absolute
relocations in data. Executable linking still follows the selected link mode.
Native shared library production has its own scope.

[#983](https://github.com/yrashk/ycxx/issues/983) · [Linux external addresses](linux-external-data-addresses.md) · [Native shared output #391](https://github.com/yrashk/ycxx/issues/391)
