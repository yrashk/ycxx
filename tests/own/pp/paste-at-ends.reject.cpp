// [cpp.concat]/1: a `##` shall not occur at the beginning or at the end of a replacement list,
// for either form of macro definition.
#define a ## x //! error
#define b x ## //! error
#define c(x) ## x //! error
#define d(x) x ## //! error
#define e(x) ## //! error
#define ok(x) x ## x
int i;
