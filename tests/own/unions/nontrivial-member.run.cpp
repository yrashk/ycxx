// #203: a written union destructor never destroys the active member implicitly.
// [class.base.init]: a union constructor initializes just its selected member.
// EXIT: 0
int constructions;
int destructions;

struct Resource {
  int value;
  Resource(int n) noexcept : value(n) { ++constructions; }
  Resource(const Resource&) = delete;
  ~Resource() noexcept { destructions += value; }
};

union Aggregate {
  Resource owner;
  long fallback;
  ~Aggregate() noexcept { destructions += 10; }
};

union Choice {
  Resource owner;
  long fallback;
  Choice() noexcept {}
  Choice(int n) noexcept : owner{n} {}
  Choice(long n) noexcept : fallback(n) {}
  ~Choice() noexcept { destructions += 100; }
};

int main() {
  {
    Aggregate aggregate{Resource{9}};
    if (aggregate.owner.value != 9 || constructions != 1) return 1;
  }
  if (destructions != 10) return 2;
  {
    Choice active(42);
    Choice inactive(5L);
    Choice empty;
    empty.fallback = 7;
    if (active.owner.value != 42 || inactive.fallback != 5 || empty.fallback != 7 || constructions != 2) return 3;
  }
  return destructions != 310;
}
