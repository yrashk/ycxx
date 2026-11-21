// A dependent reference trait selects the literal bool specialization ([temp.alias], [temp.type], #209).
// EXIT: 0
template<bool> struct Truth;

template<> struct Truth<false> {
  static constexpr int value = 17;
  static int adjust(int input) { return input + value; }
};

template<> struct Truth<true> {
  static constexpr int value = 29;
  static int adjust(int input) { return input - value; }
};

template<class T> using Trait = Truth<__is_reference(T)>;
template<class T> using Staged = Trait<T>;
template<class T> using Lvalue = T&;
template<class T> using Rvalue = T&&;

template<class T, class U> struct Same { static constexpr bool value = false; };
template<class T> struct Same<T, T> { static constexpr bool value = true; };

using Array = int[3];
using Function = long(short);

static_assert(Same<Trait<int&>, Truth<true>>::value);
static_assert(Same<Trait<int&&>, Truth<true>>::value);
static_assert(Same<Trait<void>, Truth<false>>::value);
static_assert(Same<Trait<int>, Truth<false>>::value);
static_assert(Same<Trait<const unsigned long>, Truth<false>>::value);
static_assert(Same<Trait<Array>, Truth<false>>::value);
static_assert(Same<Trait<Function>, Truth<false>>::value);
static_assert(Same<Trait<Array&>, Truth<true>>::value);
static_assert(Same<Trait<Function&>, Truth<true>>::value);

static_assert(Same<Rvalue<Lvalue<int>>, int&>::value);
static_assert(Same<Lvalue<Rvalue<int>>, int&>::value);
static_assert(Same<Rvalue<Rvalue<int>>, int&&>::value);
static_assert(Same<Staged<Rvalue<Lvalue<int>>>, Truth<true>>::value);
static_assert(Same<Staged<Lvalue<Rvalue<int>>>, Truth<true>>::value);
static_assert(Same<Staged<Rvalue<Rvalue<int>>>, Truth<true>>::value);
static_assert(Same<Staged<Array>, Truth<false>>::value);

template<class T, bool IsReference = __is_reference(T)> struct Choice;

template<class T> struct Choice<T, false> {
  static constexpr int value = Truth<false>::value;
  static int adjust(int input) { return Truth<false>::adjust(input); }
};
template<class T> struct Choice<T, true> {
  static constexpr int value = Truth<true>::value;
  static int adjust(int input) { return Truth<true>::adjust(input); }
};

template<class T> using DefaultChoice = Choice<T>;

static_assert(Same<Choice<int&>, Choice<int&, true>>::value);
static_assert(Same<Choice<int>, Choice<int, false>>::value);
static_assert(Same<DefaultChoice<Function>, Choice<Function, false>>::value);
static_assert(!Same<Choice<int&>, Choice<int&, false>>::value);
static_assert(!Same<Choice<int>, Choice<int, true>>::value);
static_assert(Choice<int&&>::value == 29);
static_assert(Choice<int&, false>::value == 17);
static_assert(Choice<void, true>::value == 29);

template<class T, bool IsReference = __is_reference(T&)>
struct ReferenceOperand : Truth<IsReference> {};

template<class T, bool IsReference = __is_reference(T*)>
struct PointerOperand : Truth<IsReference> {};

static_assert(ReferenceOperand<int>::value == 29);
static_assert(ReferenceOperand<void, false>::value == 17);
static_assert(PointerOperand<int>::value == 17);
static_assert(PointerOperand<int&, true>::value == 29);

template<class T> struct Derived : Truth<__is_reference(T)> {};

static_assert(Derived<int&&>::value == 29);
static_assert(Derived<Array>::value == 17);

int main(int argc, char**) {
  if (Derived<int&>::adjust(argc) != argc - 29)
    return 1;
  if (Derived<Function>::adjust(argc) != argc + 17)
    return 2;
  if (DefaultChoice<Array>::adjust(argc) != argc + 17)
    return 3;
  if (Choice<int&, false>::adjust(argc) != argc + 17)
    return 4;
  if (Choice<int, true>::adjust(argc) != argc - 29)
    return 5;
  return 0;
}
