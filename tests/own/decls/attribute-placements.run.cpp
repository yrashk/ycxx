// #133, from the step's review: attributes where the grammar puts them ([dcl.attr.grammar]):
// after a declarator-id ([dcl.meaning.general]/1), on a parameter ([dcl.fct]/3), on an
// alias-declaration ([dcl.pre]/1), on a statement ([stmt.pre]/1), and the GNU form on a class
// head, a block declaration, a parameter and after a template-head (docs/EXTENSIONS.md).
// EXIT: 0
struct __attribute__((packed)) Packed { int v; };
struct [[deprecated]] Old { int v; };
int k [[maybe_unused]] = 6;
int p([[maybe_unused]] int b) { return b; }
int q(__attribute__((unused)) int b) { return b; }
int r(int b __attribute__((unused))) { return b; }
using I [[maybe_unused]] = int;
template <class T>
__attribute__((noinline)) T id(T v) { return v; }
int main() {
  Packed t{1};
  __attribute__((unused)) int w = 2;
  using J [[maybe_unused]] = long;
  J j = 3;
  [[maybe_unused]] I i = 4;
  [[likely]] if (w == 2) w = 5;
  return t.v + w + j + i + p(1) + q(1) + r(1) + id(1) + k - 23;
}
