namespace fixture {
template <class T>
struct Native {
  T value;
  int select(int adjustment) & { return value + adjustment; }
  int select(int adjustment) const && { return value + adjustment + 10; }
};
extern template struct Native<int>;

template <class T>
struct Peer {
  T value;
  int select(int adjustment) &;
  int select(int adjustment) const &&;
  template <class U>
  int pick(U adjustment) &;
  template <class U>
  int pick(U adjustment) &&;
};

template <class T>
template <class U>
int Peer<T>::pick(U adjustment) & { return value + adjustment + 40; }
template <class T>
template <class U>
int Peer<T>::pick(U adjustment) && { return value + adjustment + 50; }
template int Peer<int>::pick(long) &;
template int Peer<int>::pick(long) &&;

template <class T>
int Peer<T>::select(int adjustment) & { return value + adjustment + 20; }
template <class T>
int Peer<T>::select(int adjustment) const && { return value + adjustment + 30; }
template struct Peer<int>;

int call_native() {
  Native<int> native{8};
  const Native<int> constant{9};
  if (native.select(10) != 18) return 1;
  if (static_cast<const Native<int>&&>(constant).select(11) != 30) return 2;
  return 0;
}
}
