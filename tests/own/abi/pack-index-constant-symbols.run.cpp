// #297: a constant pack index in a signature is mangled as the converted std::size_t literal,
// `Lm0E`, as GCC writes it (DECISIONS §1.7); Clang writes the index as spelled (`Li0E`). The asm
// labels name GCC's symbols, which only this object's specializations define.
// REQUIRES: darwin
// PARITY: by design: the asm labels spell Mach-O symbols, with their leading underscore
// XFAIL: clang Clang mangles the index as written, `Li0E` (DECISIONS §1.7)
// EXIT: 0
template<class... T> int two(T...[0] a, T...[1] b) { return a + b; }
template<class... T> int three(T...[0] a, T...[0] b, T...[0]* p) { return a + b + (p ? 1 : 0); }
extern "C" int two_ic(int, char) __asm__("__Z3twoIJicEEiDyT_Lm0EDyS0_Lm1E");
extern "C" int three_i(int, int, int*) __asm__("__Z5threeIJiEEiDyT_Lm0ES1_PS1_");

int main() {
  if (two<int, char>(1, 2) != 3 || three<int>(1, 2, nullptr) != 3) return 1;
  int x = 0;
  if (two_ic(4, 5) != 9 || three_i(1, 1, &x) != 3) return 2;
  return 0;
}
