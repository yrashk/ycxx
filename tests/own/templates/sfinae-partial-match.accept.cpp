// #263: deduction substitutes dependent arguments and checks the resulting types,
// including access to intermediate qualifiers ([temp.spec.partial.match], [temp.deduct.general]).
template<class T, class = void> struct HasProbe { static constexpr bool matched = false; };
template<class T> struct HasProbe<T, decltype((void)T::probe())> {
  static constexpr bool matched = true;
};

template<class T, class U> constexpr bool matches_type = false;
template<class T> constexpr bool matches_type<T, typename T::result> = true;

template<class T, class U> struct MatchesNested { static constexpr bool matched = false; };
template<class T> struct MatchesNested<T, typename T::Inner::value> {
  static constexpr bool matched = true;
};

struct Holder { using value = int; };
struct Public {
  static int probe();
  using result = int;
  using Inner = Holder;
};
class Private { using Inner = Holder; };
struct Missing {};

static_assert(HasProbe<Public>::matched);
static_assert(!HasProbe<Missing>::matched);
static_assert(!HasProbe<int>::matched);
static_assert(matches_type<Public, int>);
static_assert(!matches_type<Public, long>);
static_assert(MatchesNested<Public, int>::matched);
static_assert(!MatchesNested<Private, int>::matched);
