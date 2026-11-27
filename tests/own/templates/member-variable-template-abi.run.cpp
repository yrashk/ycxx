// #340: specializations of static data member templates are objects named as the Itanium ABI
// says (§5.1.2: nested-name, unqualified name, template-args): the peer, compiled by the oracle,
// reads and writes the ones this test instantiates ([temp.mem], [temp.inst]/7). ycxx's
// definitions are not weak yet (#124); the oracle's weak ones resolve to them.
// PEER: member-variable-template-abi-peer.cpp
// EXIT: 0
template <class C> struct S {
  template <class T> inline static int counter = sizeof(T) + sizeof(C);
};
struct N {
  template <class T> inline static long k = 7;
  template <class T> static const int w;
};
template <class T> const int N::w = sizeof(T) * 2;
namespace ns {
template <int I> struct Q {
  template <class T> static constexpr long z = I;
};
}
int peer_counter();          // reads S<int>::counter<char>
void peer_bump();            // increments S<int>::counter<char> and N::k<short>
long peer_q();               // reads ns::Q<3>::z<S<int>> through a pointer
int peer_w();                // reads N::w<short>
int* counter_address() { return &S<int>::counter<char>; }
long* k_address() { return &N::k<short>; }
const long* q_address() { return &ns::Q<3>::z<S<int>>; }
const int* w_address() { return &N::w<short>; }
int main() {
  if (*counter_address() != 5 || peer_counter() != 5) return 1;
  peer_bump();
  if (S<int>::counter<char> != 6 || N::k<short> != 8) return 2;
  if (peer_q() != 3 || *q_address() != 3 || *k_address() != 8) return 3;
  return peer_w() == 4 && *w_address() == 4 ? 0 : 4;
}
