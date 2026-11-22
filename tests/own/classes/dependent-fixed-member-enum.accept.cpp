// [temp.dep.constexpr], [temp.inst], [dcl.enum]
template<int N>
struct Fixed {
  enum State : short {
    first = N,
    second = first + 2,
    third
  };
};

static_assert(Fixed<-4>::first == -4);
static_assert(Fixed<7>::State::second == 9);
static_assert(Fixed<-4>::second == -2);
static_assert(Fixed<7>::third == 10);
static_assert(Fixed<-4>::third == -1);
static_assert(sizeof(Fixed<7>::State) == sizeof(short));
