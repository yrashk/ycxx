// #209 [basic.lookup.qual], [temp.dep.constexpr], [temp.arg.nontype], [temp.inst].
template<int Slots>
struct Budget {};

struct Missing {};

template<class Config>
struct Allocation {
  using Selected = Budget<Config::slots>; //! error
};

Allocation<Missing>* pending = nullptr;
static_assert(sizeof(Allocation<Missing>) > 0);
