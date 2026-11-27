# Leaf arm64 Mach-O dynamic libraries

The first producer slice of #393 lets the native compiler and linker supply a
freestanding library to the rule worker. The Mach-O executable writer already
has segment layout, a deterministic export trie and ad-hoc SHA-256 signing.
The shared image uses the same machinery with a library load command and no
startup entry point.

## Driver and inputs

```sh
ycxx cxx --target=arm64-apple-darwin -shared -fPIC -nostdlib \
  -Wl,-install_name,@loader_path/libexample.dylib -o libexample.dylib example.cc
```

The Darwin spelling `-dynamiclib` selects the same mode. The install name is
the output basename by default. An explicit name may be absolute or contain a
dyld path token. The program's link query includes the name and the output
basename used as the signature identifier. The driver accepts source and
ordinary arm64 object inputs. Runtime archives and other shared inputs are
outside this leaf contract. The native executable linker accepts
[explicit leaf dylib inputs](darwin-dylib-input.md) in #392.
Use `-nostdinc++ -ffreestanding` for a rule provider that includes `rule_api.h`,
as the bundled rule package does for native compilation. This selects the
platform C headers and avoids introducing libycxx runtime references.

The linker accepts strong, default-visible function exports from executable
text. Local and hidden definitions can participate in direct position-independent
branches and page-relative references, including references to immutable string
data. The linker preserves validated PC-relative DWARF unwind records with
ordinary `zR` CIEs. It rejects cleanup or personality records, imported symbols,
weak/coalesced definitions, exported data, writable sections, load-time
relocations, and initializers.

## Image and verification

`MH_DYLIB` has `__TEXT` at preferred address and file offset zero. The exported
addresses are relative to that header. `LC_ID_DYLIB` carries the install name.
The executable page-zero segment, dyld executable path and entry command are
omitted. Native macOS requires a libSystem load command even for this image
with no imported symbols. The writer emits that command without reading a
target SDK. It also emits the build version, UUID, export trie and symbol tables,
then signs the final image with its existing ad-hoc CodeDirectory and 4 KiB hash
slots. The executable segment range uses the actual `__TEXT` segment and clears
the main-binary flag. An independent design review checked these choices against
the existing layout, export and signing paths. [Apple install-name guidance](https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/DynamicLibraries/100-Articles/RunpathDependentLibraries.html) · [Native loader probe](https://github.com/yrashk/ycxx/actions/runs/46044567447)

Cross-target tests inspect the image and link-query inputs and compare cold and
resident bytes. Native arm64 macOS additionally verifies the code signature,
calls an export through `dlopen`, links and runs a platform executable that uses
the install name, and invokes `ycxx_rule_plan_v1` in the real worker.
