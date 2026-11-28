// Copy-initialization excludes known explicit constructors and constructor templates before
// deduction and constraint satisfaction ([over.match.copy]/1.1, [over.match.viable]/2-3, #1550).
template<class T> struct Trap {
  static_assert(!__is_same(T, int));
  static constexpr bool value = true;
};

struct Choice {
  int tag;
  template<class T> requires Trap<T>::value explicit constexpr Choice(T) : tag(1) {}
  constexpr Choice(long) : tag(2) {}
};

struct ExplicitTrue {
  int tag;
  template<class T> requires Trap<T>::value explicit(true) constexpr ExplicitTrue(T) : tag(1) {}
  template<class T> requires Trap<T>::value explicit(sizeof(int) > 0) constexpr ExplicitTrue(T, int = 0) : tag(3) {}
  constexpr ExplicitTrue(long) : tag(2) {}
};

struct OutOfClass {
  int tag;
  template<class T> requires Trap<T>::value explicit constexpr OutOfClass(T);
  constexpr OutOfClass(long) : tag(2) {}
};
template<class T> requires Trap<T>::value constexpr OutOfClass::OutOfClass(T) : tag(1) {}

struct Variadic {
  int tag;
  template<class... Ts> requires (Trap<Ts>::value && ...) explicit constexpr Variadic(Ts...) : tag(1) {}
  constexpr Variadic(long) : tag(2) {}
};

template<class U> struct Enclosing {
  int tag;
  template<class T> requires Trap<T>::value explicit(!__is_same(U, void)) constexpr Enclosing(T) : tag(1) {}
  explicit constexpr Enclosing(U) requires Trap<U>::value : tag(3) {}
  constexpr Enclosing(U, int) requires Trap<U>::value : tag(4) {}
  constexpr Enclosing(long) : tag(2) {}
};

template<class U> struct TwoParam {
  int tag;
  constexpr TwoParam(U, int) requires Trap<U>::value : tag(1) {}
  constexpr TwoParam(long) : tag(2) {}
};

struct NoFallback {
  template<class T> requires Trap<T>::value explicit NoFallback(T) {}
};

constexpr int take_choice(Choice c) { return c.tag; }
constexpr Choice make_choice() { return 1; }
constexpr int default_choice(Choice c = 1) { return c.tag; }
void take_no_fallback(NoFallback);

struct Agg { Choice c; };

template<class T> concept CanTake = requires(T t) { take_no_fallback(t); };
static_assert(!CanTake<int>);

void check_agg() {
  Choice arr[] = {1};
  Agg agg = {1};
  (void)arr;
  (void)agg;
}

constexpr bool check_all() {
  Choice a = 1;
  if (a.tag != 2) return false;
  if (take_choice(1) != 2) return false;
  if (make_choice().tag != 2) return false;
  if (default_choice() != 2) return false;
  const Choice& r = 1;
  if (r.tag != 2) return false;

  ExplicitTrue et = 1;
  if (et.tag != 2) return false;
  OutOfClass ooc = 1;
  if (ooc.tag != 2) return false;
  Variadic var = 1;
  if (var.tag != 2) return false;

  Enclosing<int> enc_copy = 1;
  if (enc_copy.tag != 2) return false;
  TwoParam<int> tp_copy = 1;
  if (tp_copy.tag != 2) return false;
  TwoParam<int> tp_direct(1L);
  if (tp_direct.tag != 2) return false;
  return true;
}

template<class A, class B> constexpr bool same_v = false;
template<class A> constexpr bool same_v<A, A> = true;

template<class T> constexpr bool run_local() {
  struct S {
    constexpr S(int) requires (!same_v<T, int>) {}
  };
  struct Caller {
    static char f(S);
    static short f(...);
  };
  return sizeof(Caller::f(1)) == sizeof(short);
}

static_assert(check_all());
static_assert(run_local<int>());
