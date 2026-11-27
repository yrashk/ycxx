# Explicit arm64 Mach-O dylib inputs

```sh
ycxx cxx --target=arm64-apple-darwin -nostdlib++ \
  main.cpp staging/libprovider.dylib -o app
```

The explicit path locates the link-time input. The executable records the
library's `LC_ID_DYLIB` install name and its compatibility and current versions.
For example, give the provider the install name
`@loader_path/installed/libprovider.dylib` and put it at that location relative
to `app` before running the executable. Absolute names and plain basenames are
also accepted.

The reader checks for a thin arm64 macOS `MH_DYLIB` and reads its export trie.
Strong regular exports must point into file-backed executable instructions.
The first slice accepts a dependency on macOS libSystem and diagnoses other
dependent libraries, special exports, malformed input, and unsupported install
name path tokens. Library search, rpaths, transitive dependencies, and shared
output that imports from dylibs remain tracked in #392.

The executable linker keeps libSystem at ordinal 1, followed by explicit dylibs
in command-line order and the implicit C++ ABI library when needed. Explicit
install names must be unique. Without SDK export metadata, an explicit-input
link uses the verified provider exports to resolve symbols. The normal libycxx
runtime is still selected unless the caller supplies `-nostdlib++`.

The driver reads each explicit library once per request. Its parsed metadata
and content hash enter the resident program key. A byte change reruns the link
even when the executable's import metadata and output bytes stay identical.

The acceptance fixture checks the link metadata and resident trace on both
hosts. On arm64 macOS it also runs the executables against a platform-built
provider and a native ycxx leaf from #393.
