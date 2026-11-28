// #340 item 3: a conversion function template's specialization is named as the Itanium ABI says
// (§5.1.3: `cv <type>` written in terms of the template's own parameters, then its arguments).
// The peer, compiled by the oracle, defines the specializations this test calls.
// PEER: abi-peer.cpp
// EXIT: 0
struct S {
  int n;
  template <class T> operator T() const;
};
template <class T> struct R {
  T t;
  template <class U> operator R<U>() const&;
};
int main() {
  S s{4};
  long l = s;
  R<int> a{1};
  R<long> b = a;
  return l == 5 && b.t == 3 ? 0 : 1;
}
