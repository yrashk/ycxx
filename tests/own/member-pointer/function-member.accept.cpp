// [dcl.mptr] (#238)
struct Owner;

using FunctionMember = void (Owner::*)(double) const & noexcept;

extern FunctionMember callable;
extern void (Owner::* callable)(double) const & noexcept;
void take_callable(void (Owner::* parameter)(double) const & noexcept);
