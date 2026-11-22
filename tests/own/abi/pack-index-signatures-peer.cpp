// The other side of pack-index-signatures.run.cpp, compiled by Clang: declarations only.
template<int I, class... T> T...[I] at(T... t);
template<int I, class... T> int pick(T...[I] x, T...[I]* p, T... t);
template<int I> struct A { template<class... U> static U...[I] f(U... u); };

int peer() {
  int x = 4;
  if (at<1>(1.5, 2) != 7 || at<0>(3, 'c') != 7) return 11;
  if (pick<0, int, char>(4, &x, 1, 'a') != 5) return 12;
  if (A<1>::f(1, 'c') != 9) return 13;
  return 0;
}
