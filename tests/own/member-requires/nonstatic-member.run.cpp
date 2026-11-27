// #1417 [dcl.decl.general], [class.mem.general], [over.match.viable]: a non-static member
// function of a class template with a trailing requires-clause is viable only when its
// constraints are satisfied; two overloads with complementary clauses select one each.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Box {
  T value;
  int kind() requires Small<T> { return 1; }
  int kind() requires (!Small<T>) { return 2; }
};

int main() {
  Box<char> c{'a'};
  Box<long long> l{7};
  if (c.kind() != 1) return 1;
  if (l.kind() != 2) return 2;
  return 0;
}
