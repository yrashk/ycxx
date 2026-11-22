// #839 [temp.pre], [temp.spec.partial].
template<class T>
  requires (sizeof(T) >= 2) && (sizeof(T) < 5)
struct Window { static constexpr int value = 11; };

template<class T>
  requires (sizeof(T) == 1) || (sizeof(T) == 3)
struct Choices { static constexpr int value = 17; };

static_assert(Window<char[2]>::value == 11);
static_assert(Choices<char[1]>::value == 17);
static_assert(Choices<char[3]>::value == 17);

template<class T>
struct Select { static constexpr int value = 0; };

template<class T>
  requires (sizeof(T) == 1) || (sizeof(T) == 3) && (sizeof(T) > 1)
struct Select<T> { static constexpr int value = 23; };

static_assert(Select<char[1]>::value == 23);
static_assert(Select<char[3]>::value == 23);
static_assert(Select<char[2]>::value == 0);

template<class T>
  requires (sizeof(T) >= 2) && (sizeof(T) < 5)
constexpr int function_control() { return 29; }

template<class T>
  requires (sizeof(T) == 1) || (sizeof(T) == 3)
constexpr int variable_control = 31;

static_assert(function_control<char[2]>() == 29);
static_assert(variable_control<char[3]> == 31);
