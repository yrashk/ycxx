template<class Callback>
struct Sender {
  template<class Value>
  void send(Value value) noexcept(noexcept(callback(value))) {}

  Callback callback;
};

struct ByArgument {
  void operator()(int&) noexcept;
  void operator()(long&) noexcept(false);
};

struct PotentiallyThrowing {
  void operator()(int&) noexcept(false);
};

Sender<ByArgument> by_argument;
Sender<PotentiallyThrowing> potentially_throwing;

static_assert(noexcept(by_argument.send(1)));
static_assert(!noexcept(by_argument.send(1L)));
static_assert(!noexcept(potentially_throwing.send(1)));
