// δ s4 (#86): unscoped enumerations: implicit values counting up from 0 or from the previous
// enumerator, explicit values, an enumerator used in a later enumerator's value, conversion to
// int by integral promotion, comparison, enumerators in constant expressions
// ([dcl.enum], [conv.prom], [expr.const]).
// EXIT: 0
enum Color { red, green, blue };
enum Http { ok = 200, created, accepted, not_found = 404, gone = not_found + 6 };
enum Flags { none = 0, read = 1, write = 2, exec = 4, all = read | write | exec };

static_assert(blue == 2);
static_assert(accepted == 202);

int weight(int v) { return v * 10; }

int main() {
  Color c = green;
  if (c != 1) return 1;
  if (c == red || c == blue) return 2;
  if (!(red < green && green < blue)) return 3;
  int i = blue;
  if (i != 2) return 4;
  if (weight(c) != 10) return 5;
  if (created != 201) return 6;
  if (gone != 410) return 7;
  if (all != 7) return 8;
  int perms = read | exec;
  if ((perms & write) != 0 || (perms & exec) == 0) return 9;
  c = blue;
  if (c + 1 != 3) return 10;
  Color d = c;
  if (d != blue) return 11;
  int counts[3] = {};
  counts[green] = 5;
  if (counts[1] != 5) return 12;
  long l = not_found;
  if (l != 404) return 13;
  return 0;
}
