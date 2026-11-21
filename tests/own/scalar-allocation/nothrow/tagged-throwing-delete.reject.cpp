// [basic.stc.dynamic.deallocation] A deallocation function must be nonthrowing.
// XFAIL: clang,gcc Both peers accept this potentially throwing placement deallocation declaration.
struct Token;
void operator delete(void*, const Token&) noexcept(false); //! error
