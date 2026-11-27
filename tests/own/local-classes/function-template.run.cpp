// Local classes (#117), spec-only: a local class in a function template is instantiated with the
// function; each specialization has its own distinct local class, which can use the template
// parameter for a member type ([temp.inst], [temp.local], [class.local]).
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };

template <class T> auto make(T v) {
  struct Holder {
    T value;
    T twice() const { return value + value; }
  };
  return Holder{v};
}

template <class T> int size_of_local() {
  struct L { T a; T b; };
  return sizeof(L);
}

int main() {
  auto i = make(21);
  auto d = make(1.5);
  if (i.twice() != 42) return 1;
  if (d.twice() != 3.0) return 2;
  if (same<decltype(i), decltype(d)>::value) return 3;
  if (!same<decltype(i), decltype(make(7))>::value) return 4;
  if (!same<decltype(i.value), int>::value) return 5;
  if (!same<decltype(d.value), double>::value) return 6;
  if (size_of_local<char>() != 2) return 7;
  if (size_of_local<long long>() != 2 * sizeof(long long)) return 8;
  return 0;
}
