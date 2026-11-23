// #1053 [meta.unary.prop], [basic.types.general], [class.prop].
struct Packet { int value; };
class PrivatePacket { int value; };
struct WithDestructor {
  int value;
  ~WithDestructor() {}
};
struct Pending;
enum class Code { ready };
using Function = int(double);
using OpenRows = const int[][2];

static_assert(__builtin_is_implicit_lifetime(const volatile int));
static_assert(__builtin_is_implicit_lifetime(const Code));
static_assert(__builtin_is_implicit_lifetime(Pending*));
static_assert(__builtin_is_implicit_lifetime(int Packet::*));
static_assert(__builtin_is_implicit_lifetime(decltype(nullptr)));

static_assert(__builtin_is_implicit_lifetime(Packet));
static_assert(__builtin_is_implicit_lifetime(const PrivatePacket));
static_assert(!__builtin_is_implicit_lifetime(WithDestructor));

static_assert(__builtin_is_implicit_lifetime(OpenRows));
static_assert(__builtin_is_implicit_lifetime(Pending[2]));
static_assert(__builtin_is_implicit_lifetime(const Pending[]));
static_assert(__builtin_is_implicit_lifetime(WithDestructor[2]));
static_assert(__builtin_is_implicit_lifetime(WithDestructor[]));

static_assert(!__builtin_is_implicit_lifetime(const volatile void));
static_assert(!__builtin_is_implicit_lifetime(Function));
static_assert(!__builtin_is_implicit_lifetime(Packet&));
