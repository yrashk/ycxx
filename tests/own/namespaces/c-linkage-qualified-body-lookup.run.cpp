// A qualified function body starts lookup in its target namespace ([basic.lookup.unqual]).
int helper(int) { return 1; }
namespace n {
int helper(int x) { return x + 1; }
extern "C" int ycxx_body(int);
}
int n::ycxx_body(int x) { return helper(x); }
int main() { return n::ycxx_body(41) - 42; }
