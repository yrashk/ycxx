// Local classes (#117), spec-only: a constexpr function with a deduced return type returns an object
// of its local class; callers name the type through auto and decltype and use it in constant
// expressions ([dcl.spec.auto], [dcl.constexpr], [class.local]).
constexpr auto make_pair(int a, int b) {
  struct Pair {
    int first;
    int second;
    constexpr int sum() const { return first + second; }
  };
  return Pair{a, b};
}

consteval auto make_triple() {
  struct Triple { int x, y, z; };
  return Triple{1, 2, 3};
}

using PairT = decltype(make_pair(0, 0));

constexpr auto p = make_pair(3, 4);
static_assert(p.sum() == 7);
static_assert(make_triple().z == 3);
static_assert(sizeof(PairT) == 2 * sizeof(int));

int main() {
  PairT q{10, 20};
  if (q.sum() != 30) return 1;
  auto r = make_pair(1, 1);
  r = q;
  if (r.first != 10) return 2;
  constexpr auto t = make_triple();
  if (t.x + t.y + t.z != 6) return 3;
  return 0;
}
