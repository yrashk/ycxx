// [temp.mem.class], [temp.inst]
template<class T>
struct Deferred {
  struct Record {
    typename T::type value; //! error
  };
};

Deferred<int>::Record* pointer_only = nullptr;
static_assert(sizeof(Deferred<int>::Record) > 0);
