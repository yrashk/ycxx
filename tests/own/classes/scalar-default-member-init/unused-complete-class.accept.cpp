// [class.mem.general]: a written default sees the completed class.
struct Complete {
  int size = sizeof(Complete);
  int value = seed;
  static constexpr int seed = 42;
};

int runtime_seed();
struct Source {
  int value = runtime_seed();
};

struct Nested {
  int value = Source{}.value;
};

struct Value { int number; };
struct ClassDefault { Value value = Value{3}; };
struct UnusedDefault { int value = ClassDefault{}.value.number; };

// [temp.inst]: a dependent default is not instantiated just to determine the layout.
template<class T> struct Deferred {
  int value = T::missing;
};

static_assert(sizeof(Deferred<int>) == sizeof(int));
Deferred<int> explicit_value{7};
