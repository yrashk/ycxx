// #1172: a 128-bit argument with one integer register left. Padding it once read the argument
// list while growing it, and the compiler crashed; a short unit exposed that.
// FLAGS: -Wno-pedantic
using i = __int128;
long f(long,long,long,long,long,long,long,i);
long g(i v) { return f(1,2,3,4,5,6,7,v); }
long f(long a, long b, long c, long d, long e, long x, long y, i v) { return a + b + c + d + e + x + y + (long)v; }
int main() { return g(1000) == 1028 ? 0 : 1; }
