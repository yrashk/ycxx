// #1059 [meta.unary.prop], [class.dtor], [temp.arg.nontype].
template<bool Value>
struct Property {
  static constexpr bool value = Value;
};

template<class T>
struct VirtualDestructor : Property<__has_virtual_destructor(T)> {};

template<class T>
inline constexpr bool virtual_destructor_v = __has_virtual_destructor(T);

struct Channel {
  virtual ~Channel() noexcept;
};
struct Plain {
  int value;
  ~Plain() noexcept;
};
struct VirtualOperation {
  virtual int read() const;
};
union Storage { int value; };

static_assert(VirtualDestructor<Channel>::value);
static_assert(!VirtualDestructor<Plain>::value);
static_assert(!VirtualDestructor<const VirtualOperation>::value);
static_assert(!VirtualDestructor<int>::value);
static_assert(!VirtualDestructor<Channel*>::value);
static_assert(!VirtualDestructor<Storage>::value);

struct Endpoint;
VirtualDestructor<Endpoint>* deferred;
struct Endpoint : Channel {};

static_assert(VirtualDestructor<Endpoint>::value);
static_assert(VirtualDestructor<const Endpoint>::value);
static_assert(virtual_destructor_v<Endpoint>);
static_assert(!virtual_destructor_v<VirtualOperation>);
static_assert(__has_virtual_destructor(Endpoint));
static_assert(!__has_virtual_destructor(Plain));
static_assert(VirtualDestructor<Channel>::value);
