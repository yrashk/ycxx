// #218: a dependent `__is_base_of` in a concept asks for Derived's completeness where the
// constraint is checked ([temp.constr.atomic], [meta.rel]/2); a class that is still incomplete
// there is an error, not an unsatisfied constraint.
template<class D, class B> concept BaseOf = __is_base_of(B, D); //! error
struct A {};
struct Never;
static_assert(!BaseOf<Never, A>);
int main() {}
