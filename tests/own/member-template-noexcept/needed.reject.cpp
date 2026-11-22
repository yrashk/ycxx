template<class Callback>
struct Sender {
  Callback callback;

  template<class Value>
  void send(Value value) noexcept(noexcept(callback(value))) {}  //! error
};

struct IntOnly {
  void operator()(int&) noexcept;
};

struct NotCallable {};

Sender<NotCallable> unused;
Sender<IntOnly> int_only;

static_assert(noexcept(int_only.send(1)));

constexpr bool forced = noexcept(unused.send(1));
