#!/bin/bash
# Test a Linux-cross-produced dylib without changing a byte or building ycxx.
set -euo pipefail
output=$1
input=$2
sha=$3
mkdir -p "$output/staging" "$output/installed"
library="$output/staging/provider.dylib"
cp "$input" "$library"
printf '%s  %s\n' "$sha" "$library" | shasum -a 256 -c
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
{
  uname -a
  sw_vers
  xcodebuild -version
  printf 'SDKROOT=%s\n' "$SDKROOT"
  xcrun --sdk macosx --show-sdk-version
  /usr/bin/cc --version
  shasum -a 256 "$library"
} | tee "$output/provenance.txt"

cat > "$output/loader.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    void* image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!image) {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    int (*score)(int) = (int (*)(int))dlsym(image, "score");
    const char* (*greeting)(void) = (const char* (*)(void))dlsym(image, "greeting");
    void* hidden = dlsym(image, "hidden_score");
    if (!score || !greeting || hidden) return 3;
    int result = score(35);
    const char* text = greeting();
    printf("dlopen: score(35)=%d greeting=%s hidden_score_visible=%s\n", result, text, hidden ? "yes" : "no");
    if (result != atoi(argv[2]) || strcmp(text, "native Mach-O leaf")) return 4;
    return dlclose(image);
}
EOF
cat > "$output/consumer.c" <<'EOF'
#include <stdio.h>
#include <string.h>

extern int score(int);
extern const char* greeting(void);
int main(void) {
    const char* text = greeting();
    int result = score(35);
    printf("platform-linked: score(35)=%d greeting=%s\n", result, text);
    if (strcmp(text, "native Mach-O leaf")) return 1;
    return result;
}
EOF

failed=0
run() {
  local label="$1" expected="$2"
  shift 2
  {
    printf '$'
    printf ' %q' "$@"
    printf '\n'
  } > "$output/$label.txt"
  local status=0
  "$@" >> "$output/$label.txt" 2>&1 || status=$?
  printf '%s\n' "$status" > "$output/$label.exit"
  printf '%s expected=%s actual=%s\n' "$label" "$expected" "$status"
  if [[ "$expected" != "$status" ]]; then
    cat "$output/$label.txt"
    failed=1
  fi
}

run code-signature 0 /usr/bin/codesign --verify --strict --verbose=4 "$library"
run signature-metadata 0 /usr/bin/codesign --display --verbose=4 "$library"
run mach-header 0 /usr/bin/otool -hv "$library"
run load-commands 0 /usr/bin/otool -l "$library"
run library-dependencies 0 /usr/bin/otool -L "$library"
run named-exports 0 xcrun nm -gU "$library"
run loader-build 0 /usr/bin/cc -arch arm64 "$output/loader.c" -o "$output/loader"
if [[ $(cat "$output/loader-build.exit") == 0 ]]; then
  run dlopen 0 env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH "$output/loader" "$library" 42
  tail -n +2 "$output/dlopen.txt"
fi
run platform-build 0 /usr/bin/cc -arch arm64 "$output/consumer.c" "$library" -o "$output/system-client"
if [[ $(cat "$output/platform-build.exit") == 0 ]]; then
  run client-dependencies 0 /usr/bin/otool -L "$output/system-client"
  if ! grep -F '@loader_path/installed/libycxx-leaf.dylib' "$output/client-dependencies.txt"; then
    printf 'Expected install name missing from platform executable.\n' >&2
    failed=1
  fi
  installed="$output/installed/libycxx-leaf.dylib"
  mv "$library" "$installed"
  printf '%s  %s\n' "$sha" "$installed" | shasum -a 256 -c
  run platform-run 42 env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH "$output/system-client"
  tail -n +2 "$output/platform-run.txt"
fi
exit "$failed"
