// #263: substitution stops at the first deduction failure. For Missing,
// the failed probe prevents instantiation of Explode<Missing> ([temp.deduct.general]).
template<class T> struct Explode { using result = typename T::result; };

template<class T, class = void, class = void> struct Pick {
  static constexpr bool matched = false;
};

template<class T> struct Pick<T, decltype((void)T::probe()), typename Explode<T>::result> {
  static constexpr bool matched = true;
};

struct Valid { static int probe(); using result = void; };
struct Missing {};

static_assert(Pick<Valid>::matched);
static_assert(!Pick<Missing>::matched);
