// #133: `[[…]]` attribute-specifiers on declarations and statements, and the GNU
// `__attribute__((…))` the SDK's headers use before declarations and after declarators, are
// accepted and ignored ([dcl.attr.grammar]/6, docs/EXTENSIONS.md).
// EXIT: 0
extern "C" [[gnu::visibility("default")]] int puts(const char*) __attribute__((__nonnull__(1)));
__attribute__((__deprecated__)) int old(int v);
int old(int v) { return v; }
[[nodiscard]] int f() {
  [[maybe_unused]] int x = 1;
  return 3;
}
struct [[gnu::packed]] Tagged { int v; };
int main() {
  Tagged t{4};
  [[likely]] return old(1) + f() + t.v - 8;
}
