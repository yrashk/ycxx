// #1417 [dcl.decl.general], [temp.constr.atomic], [over.match.viable]: a trailing
// requires-clause that is a parenthesized `sizeof` comparison (an atomic constraint) and a
// conjunction of two such atomic constraints.
template<class T>
struct Bytes {
  static int size_class() requires (sizeof(T) == 1) { return 1; }
  static int size_class() requires (sizeof(T) == 2) { return 2; }
  static int size_class() requires (sizeof(T) > 2) && (sizeof(T) <= 4) { return 4; }
  static int size_class() requires (sizeof(T) > 4) { return 8; }
};

struct Five { char c[5]; };

int main() {
  if (Bytes<char>::size_class() != 1) return 1;
  if (Bytes<short>::size_class() != 2) return 2;
  if (Bytes<char[3]>::size_class() != 4) return 3;
  if (Bytes<int>::size_class() != 4) return 4;
  if (Bytes<Five>::size_class() != 8) return 5;
  return 0;
}
