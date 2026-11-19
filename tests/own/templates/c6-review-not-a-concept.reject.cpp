// ζ c6 (#88), from the step's review: a return-type-requirement's type-constraint names a concept;
// in a template that is not a substitution failure but an error ([expr.prim.req.compound]).
template<class T> concept D = requires(T t) { { t } -> Nope; }; //! error
int main() { return 0; }
