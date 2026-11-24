# Undefined weak object references on Linux x86-64

The pinned libycxx allocator uses an undefined weak object to detect whether
the heapless allocation provider was linked. An explicit GNU `weak` attribute
on an ordinary external non-inline object declaration supplies the reference
binding for that entity across its translation unit.

The ELF writer uses weak binding for the undefined symbol. The existing GOT
address expression still compares with null at runtime. A direct definition
can supply the address, and an archive member remains subject to ordinary
weak-reference extraction rules. The object definition path uses its own
binding facts.

[#984](https://github.com/yrashk/ycxx/issues/984) · [Static GOT #979](https://github.com/yrashk/ycxx/issues/979) · [GNU attribute scope #190](https://github.com/yrashk/ycxx/issues/190)
