// A weak inline body and a strong asm-labelled definition of one spelling keep a symbol each
// (#694): merging entities of one name (#1095) applies only to entities this object does not
// define.
#ifdef __linux__
#define F_SYMBOL "_Z1fv"
#else
#define F_SYMBOL "__Z1fv"
#endif
inline int f() { return 1; }
int g() __asm(F_SYMBOL);
int g() { return 2; }
int use() { return g() + f(); }
