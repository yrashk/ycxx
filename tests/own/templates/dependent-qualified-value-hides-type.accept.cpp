// #209 [basic.lookup.qual], [class.member.lookup], [temp.arg.nontype], [temp.inst].
template<int Slots>
struct Budget {};

template<class Slots>
struct TypeBudget {};

struct Base { using slots = int; };
struct Inherited : Base {};
struct Config : Base { static constexpr int slots = 3; };

static_assert(__is_same(TypeBudget<Inherited::slots>, TypeBudget<int>));
static_assert(__is_same(Budget<Config::slots>, Budget<3>));

template<class T>
struct Allocation {
  using Selected = Budget<T::slots>;
};

static_assert(__is_same(Allocation<Config>::Selected, Budget<3>));
