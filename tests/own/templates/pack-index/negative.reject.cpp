// #297: [dcl.type.pack.index]/2: the index is a converted constant expression of type std::size_t,
// so a negative one is not an index of any element.
template<class... T> int f() {
  using X = T...[-1]; //! error
  return 0;
}
int main() { return f<int, char>(); }
