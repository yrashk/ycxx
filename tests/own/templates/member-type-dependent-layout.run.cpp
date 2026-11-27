// [temp.dep.constexpr], [temp.arg.nontype], [class.mem.general], [dcl.array]
// EXIT: 0
template<int N> struct Buffer { int data[N]; };

struct Limits { int extent; };
template<int N> inline constexpr Limits size{N + 1};

template<int N> struct Arithmetic {
  Buffer<N + 1> storage;
  int guard;
};

template<int N> struct Record {
  Buffer<size<N>.extent> storage;
  int guard;
};

extern "C" int witness() {
  Arithmetic<2> small;
  Record<7> large;

  if (sizeof(small.storage.data) != 3 * sizeof(int)) return 1;
  if (sizeof(large.storage.data) != 8 * sizeof(int)) return 2;

  small.guard = 23;
  large.guard = 29;
  small.storage.data[2] = 17;
  large.storage.data[7] = 19;

  if (small.storage.data[2] != 17 || small.guard != 23) return 3;
  if (large.storage.data[7] != 19 || large.guard != 29) return 4;
  return 0;
}

int main() { return witness(); }
