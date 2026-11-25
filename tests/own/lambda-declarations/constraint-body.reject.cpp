// A lambda's body is not in the immediate context of the substitution ([expr.prim.lambda.closure],
// [temp.constr.atomic]): an error in it is no failure of the constraint.
template <class T>
concept D = requires(T t) { [](auto x) { return x.nope; }(t); };  //! error
bool satisfied = D<int>;
