// #337 (ι, review): an init-statement is an expression-statement, a simple-declaration or an
// alias-declaration ([stmt.pre]), not a using-directive or a using-declaration.
namespace N { int z = 1; }
int f() { if (using namespace N; N::z) return 1; return 0; } //! error
int g() { switch (using N::z; N::z) { default: return 1; } } //! error
int main() { return f() + g(); }
