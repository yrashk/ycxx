enum E { e0 } __attribute__((packed));
struct F { char c; } __attribute__((aligned(16)));
struct H { enum N { n0 } __attribute__((packed)) m; };
E e = e0;
char get(F& f) { return f.c; }
H::N read(H& h) { return h.m; }

typedef struct { char c; } __attribute__((aligned(16))) Aligned;
typedef enum { t0 } __attribute__((packed)) Tiny;
Aligned aligned;
Tiny tiny = t0;
struct Plain { int value; } __attribute__((unused)) plain;
