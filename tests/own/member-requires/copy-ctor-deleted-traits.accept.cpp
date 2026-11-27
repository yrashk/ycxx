// #1417 (the triviality checks of copy-ctor-deleted.run.cpp) [class.copy.ctor], [dcl.fct.def.delete], [over.match.viable], [special]: a
// constrained `= delete` copy constructor makes the class non-copyable only for the
// arguments that satisfy its constraints; the complementary `= default` one serves the rest.
// XFAIL: ycxx type-trait built-ins do not classify class template specializations yet (#218)
template<class T> concept Copyable = requires(const T& t) { T(t); };
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Guard {
  T value;
  Guard(T v) : value(v) {}
  Guard(const Guard&) requires Small<T> = default;
  Guard(const Guard&) requires (!Small<T>) = delete;
};

static_assert(Copyable<Guard<char>>);
static_assert(!Copyable<Guard<long long>>);
static_assert(__is_trivially_copyable(Guard<char>));

int main() {
  Guard<short> a(3);
  Guard<short> b(a);
  return b.value == 3 ? 0 : 1;
}
