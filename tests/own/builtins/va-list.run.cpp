// #229: `__builtin_va_list` names the target's va_list type, which the SDK's `__GNUC__` path of
// arm/_types.h makes `__darwin_va_list` (docs/EXTENSIONS.md): `char*` in Apple's arm64 ABI. A
// declaration of the same name hides it.
// REQUIRES: darwin
// PARITY: target-specific: Apple arm64 names `char*` as its va_list
// EXIT: 0
typedef __builtin_va_list darwin_va_list;
static_assert(__is_same(darwin_va_list, char*));
static_assert(__is_same(__builtin_va_list, char*));

char text[] = "va";
darwin_va_list first() { return text; }

namespace hides {
int __builtin_va_list = 3;
int value() { return __builtin_va_list; }
}  // namespace hides

int main() {
  darwin_va_list p = first();
  if (p[0] != 'v' || p[1] != 'a') return 1;
  return hides::value() == 3 ? 0 : 2;
}
