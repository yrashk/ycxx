// Preserve ordinary expressions containing concept-ids at a statement boundary.
template<class T> concept Number = __is_same(T, int);
int main() {
  (void)Number<int>;
  Number<int> ? 0 : 1;
  return 0;
}
