# Freestanding driver queries

The pinned libycxx build uses `-dumpmachine` to identify the effective target and
`-print-file-name=include` to find compiler resource headers for its freestanding
include list. Both queries use ycxx's ordinary driver option parsing, including
response files and `--target`. When both are given, `-dumpmachine` takes precedence,
as observed with GCC 16 and Clang 23.

The client answers a query before project discovery or daemon startup. A query
prints one line and has no translation units or output files. The target query
prints the canonical triple of the selected target. The include query prints the
configured primary resource directory used in that target's implicit include
search. Linux uses its own resource directory and Darwin uses the shared one.
The query reports an error when its configured directory is absent.

[#970](https://github.com/yrashk/ycxx/issues/970)
