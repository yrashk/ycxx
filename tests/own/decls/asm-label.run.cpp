// #133: a GNU asm label names the entity's symbol as written, as the SDK's `__DARWIN_ALIAS`
// labels do (docs/EXTENSIONS.md): a call through `twice` reaches the function whose symbol is
// `_twice_impl`, the name a C function `twice_impl` has on Darwin.
// REQUIRES: darwin
// PARITY: by design: Darwin's '_' symbol prefix, as the SDK's labels spell it
// EXIT: 0
extern "C" int twice(int) __asm("_" "twice_impl");
extern "C" int twice_impl(int v) { return 2 * v; }
int main() { return twice(3) - 6; }
