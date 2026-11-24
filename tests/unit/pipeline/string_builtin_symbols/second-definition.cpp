// Two entities that define one symbol: an asm label naming a C function's symbol. Both object
// writers merge entities by symbol name (#1095), so they must reject a second definition rather
// than write one over the other (Clang and GCC reject it as an assembler error).
#ifdef __linux__
#define G_SYMBOL "g"
#else
#define G_SYMBOL "_g"
#endif
extern "C" int g() { return 1; }
int f() asm(G_SYMBOL);
int f() { return 2; }
