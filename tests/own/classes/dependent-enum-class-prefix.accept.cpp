// [temp.dep.constexpr], [temp.inst], [dcl.enum]
template<class T>
struct FromSize {
  static constexpr int byte_count = sizeof(T);
  enum State {
    first = byte_count,
    second = first + 1
  };
};

using FourBytes = char[4];

static_assert(FromSize<char>::first == 1);
static_assert(FromSize<FourBytes>::second == 5);
static_assert(FromSize<char>::second == 2);
