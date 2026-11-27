// Local classes (#117), spec-only: an immediately-invoked lambda returning a local aggregate
// initializes a static constexpr auto data member of a class template; each specialization's
// lambda has its own local class ([expr.prim.lambda], [class.local], [class.static.data],
// [temp.inst]).
template <class T> struct Table {
  static constexpr auto info = [] {
    struct Info {
      int size;
      T scale;
    };
    return Info{static_cast<int>(sizeof(T)), T(3)};
  }();
};

static_assert(Table<char>::info.size == 1);
static_assert(Table<double>::info.scale == 3.0);

int main() {
  if (Table<int>::info.size != sizeof(int)) return 1;
  if (Table<int>::info.scale != 3) return 2;
  if (Table<long long>::info.size != sizeof(long long)) return 3;
  return 0;
}
