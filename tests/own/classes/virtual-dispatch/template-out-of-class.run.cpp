// #1153 [temp.mem.func]: virtual members of a class template defined outside it. Such a template
// has no key function: each specialization's table is emitted where it is used.
// EXIT: 0

int alive = 0;

template <class T, int N>
struct Counter {
  Counter();
  virtual ~Counter();
  virtual T next(T step);
  virtual int limit() const noexcept;
  T value;
};

template <class T, int N> Counter<T, N>::Counter() : value(T(N)) { ++alive; }
template <class T, int N> Counter<T, N>::~Counter() { --alive; }
template <class T, int N> T Counter<T, N>::next(T step) { return value += step * T(limit()); }
template <class T, int N> int Counter<T, N>::limit() const noexcept { return N; }

struct Doubled : Counter<long, 3> {
  ~Doubled() override { alive -= 10; }
  int limit() const noexcept override { return 6; }
};

int main() {
  {
    Counter<int, 5> c;
    Counter<int, 5>& r = c;
    if (r.next(2) != 15) return 1;
    Doubled d;
    Counter<long, 3>& q = d;
    if (q.next(1) != 9) return 2;
    if (alive != 2) return 3;
  }
  return alive == -10 ? 0 : 4;
}
