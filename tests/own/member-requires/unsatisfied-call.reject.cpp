// #1417 [over.match.viable], [temp.constr.constr]: a member function whose constraints are
// not satisfied is not a viable candidate; calling it with no other candidate is ill-formed.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Box {
  int only() requires Small<T> { return 1; }
};

int main() {
  Box<char> ok;
  Box<long long> bad;
  return ok.only() + bad.only(); //! error
}
