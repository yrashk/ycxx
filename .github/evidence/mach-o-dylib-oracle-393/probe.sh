#!/bin/bash
# Compare platform dylib output and loader behavior for an import-free ARM64 ABI leaf.
set -euo pipefail
output=$1
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$output/source" "$output/clients"
source_dir="$output/source"
clients="$output/clients"
cat > "$source_dir/leaf.cc" <<'EOF'
extern "C" const char *leaf_message() {
  static const char message[] = "ycxx ARM64 leaf";
  return message;
}
extern "C" int leaf_value(int x) { return x + 23; }
EOF
cat > "$source_dir/dlopen.cc" <<'EOF'
#include <dlfcn.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!image) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
  auto value = reinterpret_cast<int (*)(int)>(dlsym(image, "leaf_value"));
  auto message = reinterpret_cast<const char *(*)()>(dlsym(image, "leaf_message"));
  if (!value || !message) { std::fprintf(stderr, "dlsym: %s\n", dlerror()); return 4; }
  const int result = value(19);
  const char *text = message();
  std::printf("dlopen result=%d text=%s\n", result, text);
  const bool good = result == 42 && std::strcmp(text, "ycxx ARM64 leaf") == 0;
  dlclose(image);
  return good ? 0 : 5;
}
EOF
cat > "$source_dir/linked.cc" <<'EOF'
#include <cstdio>
#include <cstring>

extern "C" int leaf_value(int);
extern "C" const char *leaf_message();
int main() {
  const int result = leaf_value(19);
  const char *text = leaf_message();
  std::printf("linked result=%d text=%s\n", result, text);
  return result == 42 && std::strcmp(text, "ycxx ARM64 leaf") == 0 ? 0 : 5;
}
EOF

cxx=$(xcrun --find clang++)
export SDKROOT="$(xcrun --show-sdk-path)"
{
  uname -a
  sw_vers
  xcodebuild -version
  xcrun --show-sdk-version
  xcrun --show-sdk-path
  printf 'compiler %s\nlinker %s\n' "$cxx" "$(xcrun --find ld)"
  "$cxx" --version
} | tee "$output/provenance.txt"

run() {
  local path="$1"
  shift
  {
    printf '$'
    printf ' %q' "$@"
    printf '\n'
  } > "$path"
  local code=0
  "$@" >> "$path" 2>&1 || code=$?
  printf '%s\n' "$code" > "$path.exit"
  printf '%s exit %s\n' "${path#"$output"/}" "$code"
}

metadata() {
  local folder="$1" library="$2"
  run "$folder/header.txt" xcrun otool -hv "$library"
  run "$folder/load-commands.txt" xcrun otool -l "$library"
  run "$folder/dependencies.txt" xcrun otool -L "$library"
  run "$folder/export-symbols.txt" xcrun nm -gU "$library"
  run "$folder/signature.txt" codesign --display --verbose=4 "$library"
  run "$folder/signature-verify.txt" codesign --verify --strict --verbose=4 "$library"
  awk '$1 == "cmd" {print $2}' "$folder/load-commands.txt" > "$folder/load-kinds.txt"
  printf '  load commands: '
  paste -sd ' ' "$folder/load-kinds.txt"
}

clients_test() {
  local folder="$1" library="$2" label="$3"
  run "$folder/dlopen.txt" env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH "$clients/dlopen" "$library"
  run "$folder/linked-build.txt" "$cxx" "$source_dir/linked.cc" "$library" -o "$clients/linked-$label"
  if [[ $(cat "$folder/linked-build.txt.exit") == 0 ]]; then
    run "$folder/linked-dependencies.txt" xcrun otool -L "$clients/linked-$label"
    run "$folder/linked-run.txt" env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH "$clients/linked-$label"
  fi
}

case_build() {
  local label="$1" mode="$2" folder library
  shift 2
  folder="$output/$label"
  library="$folder/leaf.dylib"
  mkdir -p "$folder"
  local install="-Wl,-install_name,$library"
  run "$folder/linker-plan.txt" "$cxx" -### "$mode" -fPIC -nostdlib "$install" "$@" "$source_dir/leaf.cc" -o "$library"
  run "$folder/dylib-build.txt" "$cxx" "$mode" -fPIC -nostdlib "$install" "$@" "$source_dir/leaf.cc" -o "$library"
  if [[ $(cat "$folder/dylib-build.txt.exit") == 0 ]]; then
    metadata "$folder" "$library"
    clients_test "$folder" "$library" "$label"
  fi
}

"$cxx" "$source_dir/dlopen.cc" -o "$clients/dlopen"
case_build shared -shared
case_build dynamiclib -dynamiclib
case_build classic-shared -shared -Wl,-ld_classic
case_build classic-dynamiclib -dynamiclib -Wl,-ld_classic
case_build libsystem -dynamiclib -lSystem
case_build reduced-metadata -dynamiclib -Wl,-no_uuid,-no_function_starts -lSystem
case_build unsigned -dynamiclib -Wl,-no_adhoc_codesign -lSystem
if [[ $(cat "$output/unsigned/dylib-build.txt.exit") == 0 ]]; then
  folder="$output/manually-signed"
  library="$output/unsigned/leaf.dylib"
  mkdir -p "$folder"
  cp "$library" "$output/unsigned/leaf-before-signing.dylib"
  run "$folder/sign.txt" codesign --force --sign - --timestamp=none "$library"
  if [[ $(cat "$folder/sign.txt.exit") == 0 ]]; then
    metadata "$folder" "$library"
    clients_test "$folder" "$library" manually-signed
  fi
fi

if [[ $(cat "$output/libsystem/dylib-build.txt.exit") == 0 ]]; then
  folder="$output/no-load-command"
  library="$output/libsystem/leaf.dylib"
  mkdir -p "$folder"
  cp "$library" "$output/libsystem/reference-before-removal.dylib"
  run "$folder/remove-dependency.txt" python3 "$script_dir/strip-test-dependency.py" "$library"
  if [[ $(cat "$folder/remove-dependency.txt.exit") == 0 ]]; then
    run "$folder/sign.txt" codesign --force --sign - --timestamp=none "$library"
    if [[ $(cat "$folder/sign.txt.exit") == 0 ]]; then
      metadata "$folder" "$library"
      clients_test "$folder" "$library" no-load-command
    fi
  fi
fi

python3 - "$output" <<'PY'
import json
import sys
from pathlib import Path

output = Path(sys.argv[1])
scenarios = {}
for name in ('shared', 'dynamiclib', 'classic-shared', 'classic-dynamiclib',
             'libsystem', 'reduced-metadata', 'unsigned', 'manually-signed', 'no-load-command'):
    folder = output / name
    if not folder.exists():
        continue
    statuses = {path.name.removesuffix('.txt.exit'): int(path.read_text())
                for path in sorted(folder.glob('*.txt.exit'))}
    commands_path = folder / 'load-kinds.txt'
    commands = commands_path.read_text().splitlines() if commands_path.exists() else []
    dependencies = [command for command in commands if command in (
        'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB',
        'LC_LAZY_LOAD_DYLIB', 'LC_LOAD_UPWARD_DYLIB',
    )]
    signature_path = folder / 'signature.txt'
    adhoc = signature_path.exists() and 'Signature=adhoc' in signature_path.read_text()
    scenarios[name] = dict(exit_status=statuses, load_commands=commands,
                           dylib_dependencies=dependencies, adhoc=adhoc)
    print('DYLIB_ORACLE ' + json.dumps({name: scenarios[name]}, separators=(',', ':')))
(output / 'summary.json').write_text(json.dumps(scenarios, indent=2) + '\n')
PY
