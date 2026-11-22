// #297: symbols as GCC writes them where Clang differs (DECISIONS §1.7). A constant pack index in
// a signature is the converted std::size_t literal, `Lm0E`; Clang writes it as spelled (`Li0E`).
// A member template indexing its class's bound pack is `Dy J <elements> E <index>`; Clang writes a
// placeholder. The asm labels name GCC's symbols, which only this object's specializations define.
// REQUIRES: darwin
// PARITY: by design: the asm labels spell Mach-O symbols, with their leading underscore
// XFAIL: clang Clang mangles the index as written, `Li0E` (DECISIONS §1.7)
// EXIT: 0
template<class... T> int two(T...[0] a, T...[1] b) { return a + b; }
template<class... T> int three(T...[0] a, T...[0] b, T...[0]* p) { return a + b + (p ? 1 : 0); }
extern "C" int two_ic(int, char) __asm__("__Z3twoIJicEEiDyT_Lm0EDyS0_Lm1E");
extern "C" int three_i(int, int, int*) __asm__("__Z5threeIJiEEiDyT_Lm0ES1_PS1_");
template<class... T> struct S { template<int I> T...[I] get() { return T...[I](I + 1); } };
extern "C" long get_1(S<int, long>*) __asm__("__ZN1SIJilEE3getILi1EEEDyJilET_v");

int main() {
  if (two<int, char>(1, 2) != 3 || three<int>(1, 2, nullptr) != 3) return 1;
  int x = 0;
  if (two_ic(4, 5) != 9 || three_i(1, 1, &x) != 3) return 2;
  S<int, long> s;
  if (s.get<1>() != 2 || get_1(&s) != 2) return 3;
  return 0;
}
