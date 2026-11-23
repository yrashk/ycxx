// #209 [expr.prim.paren], [temp.dep.constexpr], [temp.arg.nontype], [temp.inst].
template<int Slots>
struct Budget {};

struct Compact { static constexpr int slots = 3; };
struct Roomy { static constexpr int slots = 11; };

template<class Config>
struct Allocation {
  using Selected = Budget<(Config::slots)>;
};

static_assert(__is_same(Allocation<Compact>::Selected, Budget<3>));
static_assert(__is_same(Allocation<Roomy>::Selected, Budget<11>));
