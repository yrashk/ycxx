// [temp.dep.constexpr], [temp.arg.nontype], [class.mem.general]
template<int N> struct Buffer { int data[N]; };

template<int N> struct Pair {
  Buffer<N + 1> first;
  Buffer<N + 2> second;
};

extern Pair<2> small;
extern Pair<7> large;
static_assert(sizeof(small.first.data) == 3 * sizeof(int));
static_assert(sizeof(small.second.data) == 4 * sizeof(int));
static_assert(sizeof(large.first.data) == 8 * sizeof(int));
static_assert(sizeof(large.second.data) == 9 * sizeof(int));
