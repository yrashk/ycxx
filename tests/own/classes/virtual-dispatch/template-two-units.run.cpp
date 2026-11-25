// #1153 [temp.inst], Itanium C++ ABI §5.2.3: each unit that uses a specialization emits its table,
// RTTI and members weakly, and the linker keeps one of each.
// PEER: template-two-units-peer.cpp
// EXIT: 0

template <class T> struct Base {
  Base() {}
  virtual ~Base() { value = 0; }
  virtual T get() const { return value; }
  virtual int out() const;
  T value = 3;
};
template <class T> int Base<T>::out() const { return 100; }

template <class T> struct Derived : Base<T> {
  Derived() {}
  ~Derived() {}
  T get() const override { return this->value + T(10); }
};

int peer_get(const Base<int>&);
int peer_out(const Base<int>&);
Base<int>& peer_base();
Base<int>& peer_derived();

int main() {
  Base<int> b;
  Derived<int> d;
  if (peer_get(b) != 3 || peer_get(d) != 13 || peer_out(d) != 100) return 1;
  if (peer_base().get() != 3 || peer_derived().get() != 13) return 2;
  return peer_base().out() == 100 && peer_derived().out() == 100 ? 0 : 3;
}
