// REQUIRES: syntax
// [expr.sizeof], [temp.dep.constexpr], [temp.inst]
template<class T> struct Bytes {
  unsigned char data[sizeof(T)];
};

struct Opaque;
Bytes<Opaque>* opaque();

struct Later;
Bytes<Later>* pending();

struct Later { unsigned char payload[3]; };
constexpr Bytes<Later> resolved{{5, 7, 11}};

static_assert(sizeof(resolved.data) == sizeof(Later));
static_assert(resolved.data[0] == 5 && resolved.data[2] == 11);
