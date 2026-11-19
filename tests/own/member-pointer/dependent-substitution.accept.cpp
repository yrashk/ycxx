// [dcl.mptr] (#238)
template<class M, class C>
concept R = requires(M C::* p) { p; };

struct Forward;
union NamedUnion;
using Function = void(double) const & noexcept;

static_assert(R<int, Forward>);
static_assert(R<int, NamedUnion>);
static_assert(R<Function, Forward>);
static_assert(!R<int, int>);
static_assert(!R<void, Forward>);
static_assert(!R<int&, Forward>);
