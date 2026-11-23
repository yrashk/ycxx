# Freestanding language mode

`-ffreestanding` sets `__STDC_HOSTED__` to `0` in the built-in preprocessor
definitions. The default is `1`. The pinned libycxx's `ycxx/config.hpp` uses that
macro to select its hosted facilities. The compiler and editor both parse the
option through the shared preprocessing option parser.

The mode belongs to the effective preprocessing configuration. It distinguishes
session and translation unit identities and is serialized with the source
configuration. The engine's structural schema distinguishes the added field.

Freestanding startup is implementation-defined ([basic.start.main]). A ycxx
object can omit the entry point. An x86-64 Linux executable built with
`-nostdlib` can use the existing startup that calls `main` or supply `_start`
in a static link under the same rules as other ycxx links. `-ffreestanding`
selects the language mode independently of link and include search options.

[#973](https://github.com/yrashk/ycxx/issues/973) · [Include search #251](https://github.com/yrashk/ycxx/issues/251)
