// Shared by abi.run.cpp and abi-peer.cpp: the functions each side defines.
using i128 = __int128;
using u128 = unsigned __int128;
struct Box { u128 v; };
struct Wrapped { Box b; };

u128 wide(unsigned long hi, unsigned long lo);

#define DECLARE(p)                                                                                    \
  u128 p##_first(long a, u128 b);                                                                     \
  u128 p##_second(u128 a, u128 b, long c, u128 d);                                                    \
  u128 p##_split(long a, long b, long c, long d, long e, u128 f, long g, long h);                     \
  u128 p##_stack(long a, long b, long c, long d, long e, long f, long g, u128 h, long i, u128 j);     \
  u128 p##_packed(long a, long b, long c, long d, long e, long f, long g, long h, signed char i, u128 j, \
                  short k, u128 l, bool m);                                                           \
  u128 p##_mixed(double a, u128 b, float c, u128 d, double e, u128 f, int g, u128 h);                 \
  u128 p##_box(int a, Box b, long c);                                                                 \
  u128 p##_box_stack(long a, long b, long c, long d, long e, long f, long g, Box h, long i, Box j);   \
  Box p##_box_result(Box a, i128 b);                                                                  \
  i128 p##_signed(i128 a, int b, i128 c);                                                             \
  u128 p##_many(u128 a, u128 b, u128 c, u128 d, u128 e, u128 f);
DECLARE(our)
DECLARE(peer)
