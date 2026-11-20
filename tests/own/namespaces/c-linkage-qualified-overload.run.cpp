// Redeclaring a namespace function inherits its language linkage ([dcl.link]).
namespace api {
extern "C" int ycxx_lookup(int);
int ycxx_lookup(int, int);
}
int api::ycxx_lookup(int result) { return result; }
int api::ycxx_lookup(int a, int b) { return a + b; }
int main() { return api::ycxx_lookup(1) + api::ycxx_lookup(2, 3) - 6; }
