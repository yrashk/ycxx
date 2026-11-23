// #917 [dcl.meaning], [dcl.fct], [except.spec], [stmt.ambig].
struct Source { using type = long; };

template<class Provider>
void declarations() {
  typename Provider::type (implicit_true(int) noexcept);
  typename Provider::type (explicit_true(int) noexcept(true));

  static_assert(noexcept(implicit_true(0)));
  static_assert(noexcept(explicit_true(0)));
  static_assert(__is_same(decltype(implicit_true(0)), long));
  static_assert(__is_same(decltype(explicit_true(0)), long));
}

void check() { declarations<Source>(); }
