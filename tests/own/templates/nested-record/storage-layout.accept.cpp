// [class.nest], [temp.dep.type], [basic.lookup.qual], [dcl.align]
// Mirrors the class and alias in libycxx's deprecated aligned_storage.
template<unsigned Len, unsigned Align>
struct Storage {
  struct type {
    alignas(Align) unsigned char data[Len];
  };
};

template<unsigned L, unsigned A>
using StorageType = typename Storage<L, A>::type;

using Stored = StorageType<3, 8>;
static_assert(sizeof(Stored::data) == 3);
static_assert(sizeof(Stored) == 8);
static_assert(alignof(Stored) == 8);
static_assert(__builtin_offsetof(Stored, data) == 0);
static_assert(sizeof(Stored[2]) == 16);
