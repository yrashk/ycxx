// #263: missing and inaccessible dependent names cause deduction failure
// when they appear in a partial specialization's arguments ([temp.deduct.general]).
template<class...> using void_t = void;

template<class T, class = void> struct HasValue { static constexpr bool result = false; };
template<class T> struct HasValue<T, void_t<typename T::value>> {
  static constexpr bool result = true;
};

template<class T, class U> struct MatchesValue { static constexpr bool result = false; };
template<class T> struct MatchesValue<T, typename T::value> {
  static constexpr bool result = true;
};

struct Public { using value = int; };
struct Missing {};
class Private { using value = int; };

static_assert(HasValue<Public>::result);
static_assert(!HasValue<Missing>::result);
static_assert(MatchesValue<Public, int>::result);
static_assert(!MatchesValue<Private, int>::result);
