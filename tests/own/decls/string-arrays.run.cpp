// #133: character arrays from string literals, with and without braces and a bound
// ([dcl.init.string]), arrays of unknown bound completed by a later declaration ([dcl.array]/6,
// [basic.link]/11) and of pointers, at namespace and block scope.
// EXIT: 0
char ver[] = "0.1";
inline constexpr char abi[] = "a" "bc";
char fixed[6] = "hi";
const char braced[] = {"xyz"};
unsigned char uc[] = "\xff";
extern const char* const names[];
const char* const names[] = {"one", "two"};
extern int counts[];
int counts[3] = {1, 2, 3};
int main() {
  char local[] = "local";
  char sized[8] = "ab";
  signed char sc[] = {"q"};
  return (ver[2] - '1') + (abi[2] - 'c') + fixed[5] + (braced[3]) + (uc[0] - 255) + (names[1][1] - 'w') +
         (counts[2] - 3) + (local[4] - 'l') + sized[7] + (sc[0] - 'q') - 0;
}
