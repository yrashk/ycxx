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
Bytes<Later> resolved;

static_assert(sizeof(resolved.data) == sizeof(Later));
