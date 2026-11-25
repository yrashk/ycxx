// #317, [expr.ref], [expr.cond]: both arms need valid fields before instantiation.
struct Fraction {
  long num;
  bool ok;
};

template<long> struct Scalar;

template<Fraction V> struct Project {
  using type = Scalar<(V.ok ? V.num : V.missing)>; //! error
};
