// [expr.prim.id.unqual]: an array of volatile elements is itself volatile.
using VolatileArray = volatile int[2];

struct Result {
  Result(VolatileArray&);
  Result(VolatileArray&&) = delete;
};

Result from_array() {
  VolatileArray array = {};
  return array;
}
