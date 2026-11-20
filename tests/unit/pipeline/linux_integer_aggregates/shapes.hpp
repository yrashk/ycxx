struct Pair { int a, b; };
struct Small { unsigned char a, b, c; };
struct Padded { unsigned char a; unsigned int b; };
struct Three { int a, b, c; };
struct Wide { unsigned long a, b; };

constexpr unsigned long high = 0x0123456789abcdefUL;
constexpr unsigned long low = 0xf1e2d3c4b5a69788UL;
static_assert(sizeof(Pair) == 8 && sizeof(Small) == 3 && sizeof(Padded) == 8 &&
              sizeof(Three) == 12 && sizeof(Wide) == 16);

int sum(Pair);
int sum(long);
Small rotate(Small);
unsigned padded(Padded);
Three shift(Three);
Wide flip(Wide);
int rollback(long, long, long, long, long, Wide, signed char, long);
Wide unused_register(long, long, long, long, long, Wide);
int stacked(long, long, long, long, long, long, Pair, Wide, long);
int mutate_parameter(Pair, const Pair*);
int mutate_original(Pair*, Pair);
