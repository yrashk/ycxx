// [dcl.mptr] (#238)
// XFAIL: gcc GCC 16 drops the ref-qualifier when forming a member pointer through a function alias.
struct Owner;
using QualifiedFunction = void(double) const & noexcept;

extern QualifiedFunction Owner::* callable;
extern void (Owner::* callable)(double) const & noexcept;
