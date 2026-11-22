enum E { e0 } __attribute__((unused));
struct F { char c; } __attribute__((unused));
struct H { enum N { n0 } __attribute__((unused)) m; };
E e = e0;
char get(F& f) { return f.c; }
H::N read(H& h) { return h.m; }

typedef struct { char c; } __attribute__((unused)) Alias;
typedef enum { t0 } __attribute__((unused)) Tiny;
Alias alias;
Tiny tiny = t0;
struct Plain { int value; } __attribute__((unused)) plain;

enum { ordinary = 1 } __attribute__((unused));
static_assert(ordinary == 1);
