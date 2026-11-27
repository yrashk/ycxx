// [temp.inst], [class.mem.general], [dcl.array]
template<int N> struct Buffer { int data[N]; };

template<int N> struct Wrap { Buffer<N> storage; };

extern Wrap<2> small;
extern Wrap<7> large;
static_assert(sizeof(small.storage.data) == 2 * sizeof(int));
static_assert(sizeof(large.storage.data) == 7 * sizeof(int));
