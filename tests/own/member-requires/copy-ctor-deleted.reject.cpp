// #1417 [class.copy.ctor], [dcl.fct.def.delete], [over.match.best.general]: when the
// satisfied copy constructor is the constrained `= delete` one, copying selects it and is
// ill-formed.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Guard {
  T value;
  Guard(T v) : value(v) {}
  Guard(const Guard&) requires Small<T> = default;
  Guard(const Guard&) requires (!Small<T>) = delete;
};

int main() {
  Guard<long long> a(3);
  Guard<long long> b(a); //! error
  return int(b.value);
}
