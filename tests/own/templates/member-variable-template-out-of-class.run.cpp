// #340: a static data member template of a non-template class declared in the class and
// defined at namespace scope, then used qualified and from a member function defined after it
// ([temp.mem], [class.static.data], [temp.pre]/2).
// EXIT: 0
struct N {
  template <class T> static const int w;
  template <class T> static T store;
  static int read();
};
template <class T> const int N::w = int(sizeof(T)) * 3;
template <class T> T N::store = T(7);
int N::read() { return w<int> + w<char>; }
int main() {
  if (N::w<short> != 6) return 1;
  if (N::read() != 15) return 2;
  N::store<int> += 1;
  if (N::store<int> != 8 || N::store<long> != 7) return 3;
  return &N::store<int> == &N::store<int> ? 0 : 4;
}
