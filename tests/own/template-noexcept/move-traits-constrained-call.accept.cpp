// #218, [except.spec], [temp.inst], [temp.constr.atomic], [temp.constr.order]
// Move validity admits both calls. Only the non-throwing call selects the constrained arm.
template<class T>
concept CanMove = requires(T& left, T& right) {
  T(static_cast<T&&>(right));
  left = static_cast<T&&>(right);
};

struct Exchange {
  template<class T>
    requires CanMove<T>
  void operator()(T& first, T& second) const noexcept(
      __is_nothrow_constructible(T, T&&) && __is_nothrow_assignable(T&, T&&)) {
    T temporary(static_cast<T&&>(first));
    first = static_cast<T&&>(second);
    second = static_cast<T&&>(temporary);
  }
};

extern const Exchange exchange;

struct ThrowingAssignment {
  ThrowingAssignment(ThrowingAssignment&&) noexcept;
  ThrowingAssignment& operator=(ThrowingAssignment&&) noexcept(false);
  ~ThrowingAssignment() noexcept;
};

template<class T>
concept NothrowExchange = requires(T& left, T& right) {
  requires noexcept(exchange(left, right));
};

template<class T>
constexpr int select() { return 0; }

template<class T> requires NothrowExchange<T>
constexpr int select() { return 1; }

static_assert(CanMove<int>);
static_assert(CanMove<ThrowingAssignment>);
static_assert(select<int>() == 1);
static_assert(select<ThrowingAssignment>() == 0);
