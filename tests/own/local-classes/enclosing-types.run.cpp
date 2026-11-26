// Local classes (#117), spec-only: names of types and enumerators declared earlier in the enclosing
// block (a typedef, an alias-declaration, an enumeration and its enumerators) are found from
// inside a local class ([class.local], [basic.scope.block], [basic.lookup.unqual]).
int main() {
  typedef long Wide;
  using Small = short;
  enum Color { red = 3, green = 5, blue = 7 };
  enum class Mode { on = 11, off = 13 };
  struct S {
    Wide w;
    Small s;
    Color c = green;
    Mode m = Mode::off;
    int total() const { return static_cast<int>(w) + s + c + static_cast<int>(m) + blue; }
  };
  S x{1, 2};
  if (sizeof(x.w) != sizeof(long) || sizeof(x.s) != sizeof(short)) return 1;
  if (x.c != green || x.m != Mode::off) return 2;
  if (x.total() != 1 + 2 + 5 + 13 + 7) return 3;
  x.c = red;
  if (x.total() != 1 + 2 + 3 + 13 + 7) return 4;
  return 0;
}
