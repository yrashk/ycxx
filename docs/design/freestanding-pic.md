# Explicit PIC for Linux x86-64 objects

The pinned libycxx freestanding archive requests `-fPIC` through CMake's
`POSITION_INDEPENDENT_CODE` property. The compiler driver and the editor share
the option. On x86-64 Linux it defines `__PIC__` and `__pic__` as `2`. The
effective setting is part of the source identity and the serialized mode byte.
Earlier mode values decode with the ordinary predefines. The driver diagnoses
the option for other compilation targets.

Astra reviewed reuse of the existing Linux object emission. Ordinary nonlocal
addresses use the GOT, direct calls use PLT-relative relocations, and local
addresses are RIP-relative. Static pointer initializers use absolute
relocations in data. Executable linking still follows the selected link mode.
Native shared library production has its own scope.

[#983](https://github.com/yrashk/ycxx/issues/983) · [Linux external addresses](linux-external-data-addresses.md) · [Native shared output #391](https://github.com/yrashk/ycxx/issues/391)
