// #1417 [class.copy.assign], [dcl.fct.def.delete], [over.match.viable]: a constrained
// `= delete` move assignment operator that is satisfied is selected for an rvalue and makes
// the assignment ill-formed, although a satisfied copy assignment operator exists.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Opt {
  T payload;
  Opt& operator=(const Opt&) = default;
  Opt& operator=(Opt&&) requires Small<T> = default;
  Opt& operator=(Opt&&) requires (!Small<T>) = delete;
};

int main() {
  Opt<char> a{1}, b{2};
  a = static_cast<Opt<char>&&>(b);
  Opt<int> c{1}, d{2};
  c = d;
  c = static_cast<Opt<int>&&>(d); //! error
  return 0;
}
