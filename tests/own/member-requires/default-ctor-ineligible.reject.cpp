// #1417 [class.default.ctor], [over.match.viable]: a class template whose only default
// constructor is constrained; for a T that does not satisfy the constraint there is no
// viable default constructor (no implicit one is declared), so default-initialization is
// ill-formed.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Slot {
  T v;
  Slot() requires Small<T> = default;
  Slot(T x) : v(x) {}
};

int main() {
  Slot<char> ok;
  Slot<int> fine(3);
  Slot<int> bad; //! error
  return 0;
}
