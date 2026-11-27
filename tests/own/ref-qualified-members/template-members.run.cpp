// [dcl.fct], [over.match.funcs], [temp.inst], [temp.mem.func]
// EXIT: 0
namespace fixture {
template <class T>
struct Wrap {
  T value{};

  const T& base() const& { return value; }
  T base() && { return static_cast<T&&>(value) + 100; }

  int select() & { return 1; }
  int select() const & { return 2; }
  int select() && { return 3; }
  int select() const && { return 4; }

  int outside() &;
  int outside() &&;

  template <class U>
  U convert() const& { return static_cast<U>(value); }
  template <class U>
  U convert() && { return static_cast<U>(value) + 10; }

  template <class U>
  U widen() &;
  template <class U>
  U widen() &&;

  int forward_self() & { return static_cast<Wrap&&>(*this).select(); }
  int this_self() && { return this->select(); }

  struct Inner {
    T held{};
    T get() const& { return held; }
    T get() && { return held + 1; }
  };
};

template <class T>
int Wrap<T>::outside() & { return static_cast<int>(value) + 20; }
template <class T>
int Wrap<T>::outside() && { return static_cast<int>(value) + 30; }

template <class T>
template <class U>
U Wrap<T>::widen() & { return static_cast<U>(value) + 40; }
template <class T>
template <class U>
U Wrap<T>::widen() && { return static_cast<U>(value) + 50; }

template <class T>
struct Spec {
  int f() & { return 1; }
  int f() && { return 2; }
};
template <class T>
struct Spec<T*> {
  int f() & { return 3; }
  int f() && { return 4; }
};
template <>
struct Spec<char> {
  int f() & { return 5; }
  int f() && { return 6; }
};

template <class T>
int forward_call(T&& object) { return static_cast<T&&>(object).f(); }

template <class T>
int take_rvalue(T object) { return static_cast<T&&>(object).select(); }
}

int main() {
  using fixture::Wrap;
  Wrap<int> plain{5};
  const Wrap<int> constant{6};
  if (plain.base() != 5) return 1;
  if (Wrap<int>{7}.base() != 107) return 2;
  if (plain.select() != 1) return 3;
  if (constant.select() != 2) return 4;
  if (Wrap<int>{}.select() != 3) return 5;
  if (static_cast<const Wrap<int>&&>(constant).select() != 4) return 6;
  if (plain.outside() != 25) return 7;
  if (Wrap<int>{1}.outside() != 31) return 8;
  if (plain.convert<long>() != 5) return 9;
  if (Wrap<int>{2}.convert<long>() != 12) return 10;
  if (plain.forward_self() != 3) return 11;
  if (Wrap<int>{}.this_self() != 1) return 12;
  Wrap<int>::Inner inner{8};
  if (inner.get() != 8) return 13;
  if (Wrap<int>::Inner{9}.get() != 10) return 14;
  if (fixture::take_rvalue(plain) != 3) return 15;
  Wrap<long> wide{3};
  if (wide.base() != 3 || Wrap<long>{3}.base() != 103) return 16;
  if (plain.widen<long>() != 45 || Wrap<int>{1}.widen<long>() != 51) return 17;
  fixture::Spec<int> primary;
  fixture::Spec<int*> partial;
  fixture::Spec<char> explicit_;
  if (primary.f() != 1 || fixture::Spec<int>{}.f() != 2) return 18;
  if (partial.f() != 3 || fixture::Spec<int*>{}.f() != 4) return 19;
  if (explicit_.f() != 5 || fixture::Spec<char>{}.f() != 6) return 20;
  if (fixture::forward_call(primary) != 1 || fixture::forward_call(fixture::Spec<int>{}) != 2) return 21;
  return 0;
}
