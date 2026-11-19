// ζ c6 (#88), from the step's review: a return-type-requirement may name a qualified concept
// ([expr.prim.req.compound], [temp.param]/2).
// EXIT: 0
namespace N { template<class T, class U> concept Same = __is_same(T, U); }
template<class T> concept C = requires(T t) { { t + t } -> N::Same<int>; };
static_assert(C<int> && !C<long>);
int main() { return 0; }
