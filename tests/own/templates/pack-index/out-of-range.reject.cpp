// #297: [dcl.type.pack.index]/2: the index is less than the number of elements.
template<class... T> int f() {
  using X = T...[2]; //! error
  return 0;
}
int main() { return f<int, char>(); }
