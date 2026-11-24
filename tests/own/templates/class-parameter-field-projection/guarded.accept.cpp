// #317, [expr.cond], [temp.type], [temp.expl.spec]: guarded projections select
// the same scalar specialization as literal values.
struct Fraction {
  long num;
  long den;
  bool ok;
};

template<long, long> struct Ratio;

template<Fraction V> struct Project {
  using type = Ratio<(V.ok ? V.num : 0), (V.ok ? V.den : 1)>;
};

template<> struct Project<Fraction{9, 0, false}> {
  using type = Ratio<7, 3>;
};

constexpr Fraction named{-4, 3, true};
static_assert(__is_same(Project<named>::type, Ratio<-4, 3>));
static_assert(__is_same(Project<Fraction{-4, 3, true}>::type, Ratio<-4, 3>));
static_assert(__is_same(Project<Fraction{3, -4, true}>::type, Ratio<3, -4>));
static_assert(__is_same(Project<Fraction{-4, 3, false}>::type, Ratio<0, 1>));
static_assert(__is_same(Project<Fraction{9, 0, false}>::type, Ratio<7, 3>));
