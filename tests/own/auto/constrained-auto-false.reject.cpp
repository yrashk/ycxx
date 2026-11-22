// Check an ordinary false constraint on a successfully deduced type.
template<class T> concept IsInt = __is_same(T, int);
int main() {
  IsInt auto value = 1.0; //! error
  return 0;
}
