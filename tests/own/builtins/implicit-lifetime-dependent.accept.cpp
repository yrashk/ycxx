// #1053 [meta.unary.prop], [basic.types.general], [class.prop], [temp.arg.nontype].
template<bool Value>
struct LifetimeFlag {
  static constexpr bool value = Value;
};

template<class T>
struct Lifetime : LifetimeFlag<__builtin_is_implicit_lifetime(T)> {};

template<class T>
inline constexpr bool lifetime_v = __builtin_is_implicit_lifetime(T);

struct Packet { int value; };
class PrivatePacket { int value; };
struct WithDestructor {
  int value;
  ~WithDestructor() {}
};
struct Later;
Lifetime<Later>* deferred;

static_assert(Lifetime<Packet>::value && lifetime_v<Packet>);
static_assert(!Lifetime<WithDestructor>::value && !lifetime_v<WithDestructor>);
static_assert(Lifetime<const PrivatePacket>::value && lifetime_v<const PrivatePacket>);
static_assert(Lifetime<WithDestructor[2]>::value && lifetime_v<WithDestructor[]>);
static_assert(Lifetime<Later[2]>::value && lifetime_v<Later[]>);
static_assert(Lifetime<int>::value && lifetime_v<const int>);
static_assert(!Lifetime<int&>::value && !lifetime_v<int&>);

struct Later { int value; };
static_assert(Lifetime<Later>::value && lifetime_v<Later>);
static_assert(Lifetime<Packet>::value && lifetime_v<Packet>);
