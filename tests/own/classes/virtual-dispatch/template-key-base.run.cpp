// #1153: the table of an ordinary class with a key function names members of its class template
// specialization base that nothing else in the unit uses ([temp.inst], Itanium C++ ABI §5.2.3).
// PEER: template-key-base-peer.cpp
// EXIT: 0

template <class T> struct Base {
  virtual ~Base() {}
  virtual int unused() { return int(sizeof(T)); }
  virtual int key() { return 0; }
};
struct Derived : Base<short> {
  Derived();
  int key() override;
};
int Derived::key() { return 5; }

int call(Base<short>&);          // the peer's
Base<short>& made_by_peer();

int main() {
  Base<short> here;
  if (call(here) != 2 + 0) return 1;
  Base<short>& there = made_by_peer();
  return there.unused() == 2 && there.key() == 5 ? 0 : 2;
}
