// ι (#300): pointer parameters defaulted to nullptr, to a string literal, and to the address of
// a global ([dcl.fct.default]/1, [conv.ptr], [lex.string]).
// EXIT: 0
int value = 7;

int deref(const int* p = nullptr) { return p ? *p : -1; }
int length(const char* s = "hello") {
  int n = 0;
  while (s[n]) ++n;
  return n;
}
char first(const char* s = "xyz") { return s[0]; }
int target(int* p = &value) { return *p; }

int main() {
  if (deref() != -1) return 1;
  int k = 3;
  if (deref(&k) != 3) return 2;
  if (length() != 5) return 3;
  if (length("ab") != 2) return 4;
  if (first() != 'x' || first("q") != 'q') return 5;
  if (target() != 7) return 6;
  value = 8;
  if (target() != 8) return 7;
  if (target(&k) != 3) return 8;
  return 0;
}
