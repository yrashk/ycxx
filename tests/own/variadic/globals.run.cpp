// Variables with static storage duration passed through a C ellipsis are passed by value, after
// the lvalue-to-rvalue and array-to-pointer conversions ([expr.call]/12; #184), and so is a data
// member read inside a member function.
// STDOUT: 5 -9 1099511627776
// STDOUT: global ptr
// STDOUT: 6 12
// EXIT: 0
extern "C" int printf(const char*, ...);

int g = 5;
short gs = -9;
long gl = 1099511627776L;
char text[7] = {'g', 'l', 'o', 'b', 'a', 'l', 0};
const char* gp = "ptr";

struct Box {
  int m;
  int show() { return printf("%d %d\n", m, m * 2); }
};

int main() {
  printf("%d %d %ld\n", g, gs, gl);
  printf("%s %s\n", text, gp);
  Box b{6};
  return b.show() - 5;
}
