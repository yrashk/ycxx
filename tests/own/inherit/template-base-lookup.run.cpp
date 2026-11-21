// Unqualified lookup ignores a dependent base, including during member replay ([temp.dep]).
// EXIT: 0
constexpr int value = 1;
template<class T> struct Base { static constexpr int value = 2; };
template<class T> struct Derived : Base<T> {
  static constexpr int initial = value;
  static constexpr int unqualified() { return value; }
  static constexpr int qualified() { return Derived::value; }
};
static_assert(Derived<int>::initial == 1);
static_assert(Derived<int>::value == 2);

struct Ordinary { static constexpr int value = 17; };
template<class T> struct FromOrdinary : Ordinary {
  static constexpr int initial = value;
  static constexpr int unqualified() { return value; }
};
static_assert(FromOrdinary<int>::initial == 17);
static_assert(FromOrdinary<long>::value == 17);

template<class T> struct Hides : T { static constexpr int value = 29; };
static_assert(Hides<Ordinary>::value == 29);

int main() {
  return Derived<int>::unqualified() == 1 && Derived<int>::qualified() == 2 &&
                 FromOrdinary<int>::unqualified() == 17 ? 0 : 1;
}
