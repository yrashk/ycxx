// ι (#94), from the review: the elements of a namespace-scope array of class type, of two
// dimensions, are constructed in order and destroyed in reverse ([basic.start.term], [class.dtor]).
// STDOUT: M543210
// EXIT: 0
extern "C" int putchar(int);
int made = 0;
struct E { int c; E() : c(made++) {} ~E() { putchar('0' + c); if (c == 0) putchar('\n'); } };
E grid[2][3];
int main() { putchar('M'); return grid[1][2].c == 5 ? 0 : 1; }
