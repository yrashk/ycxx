// [temp.mem.class], [temp.inst]
template<class T>
struct Deferred {
  struct Record {
    typename T::type value;
  };
};

struct Valid {
  using type = unsigned char;
};

Deferred<int>::Record* pointer_only = nullptr;
static_assert(sizeof(Deferred<Valid>::Record) == 1);
