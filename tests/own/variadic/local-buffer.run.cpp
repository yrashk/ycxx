// A block-scope array passed through a C ellipsis decays to a pointer ([expr.call]/12,
// [conv.array]); Apple arm64 passes it on the stack (#189). The arrays are intact afterwards.
// STDOUT: hi
// STDOUT: abc|hi
// STDOUT: 7 8
// EXIT: 0
extern "C" int printf(const char*, ...);

int main() {
  char buf[8] = {'h', 'i', 0};
  char abc[4] = {97, 98, 99, 0};
  int n[2] = {7, 8};
  printf("%s\n", buf);
  printf("%s|%s\n", abc, buf);
  int* p = n;
  printf("%d %d\n", n[0], p[1]);
  return (abc[1] - 98) + (buf[1] - 'i') + (n[1] - 8);
}
