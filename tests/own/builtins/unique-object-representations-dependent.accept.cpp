// #1062 [meta.unary.prop], [basic.fundamental], [temp.arg.nontype].
template<bool Value>
struct Property {
  static constexpr bool value = Value;
};

template<class T>
struct Unique : Property<__has_unique_object_representations(T)> {};

template<class T>
inline constexpr bool unique_v = __has_unique_object_representations(T);

using Bytes = const unsigned char[2][3];
using OpenBytes = volatile char[][3];
using OpenFloats = const double[][2];
using Function = void();

static_assert(Unique<Bytes>::value && unique_v<Bytes>);
static_assert(Unique<OpenBytes>::value && unique_v<OpenBytes>);
static_assert(!Unique<OpenFloats>::value && !unique_v<OpenFloats>);
static_assert(!Unique<Function>::value && !unique_v<Function>);
static_assert(!Unique<const void>::value && !unique_v<const void>);

struct Later;
Unique<Later>* deferred;
struct Later { char value; };
static_assert(!Unique<Later&>::value && !unique_v<Later&>);
static_assert(Unique<Bytes>::value && unique_v<Bytes>);
