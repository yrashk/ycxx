// ζ s3 (#88), from the step's review: partial specializations for `const T` are not the primary
// template's own arguments, and the most specialized matching one is used
// ([temp.spec.partial.general], [temp.spec.partial.order]).
// EXIT: 0
template<class T> struct is_const { int g() { return 0; } };
template<class T> struct is_const<const T> { int g() { return 1; } };
template<class T> constexpr int r = 0;
template<class T> constexpr int r<T*> = 1;
template<class T> constexpr int r<const T> = 2;
template<class T> int use() { return r<T>; }
int main() {
  if (is_const<const int>{}.g() != 1 || is_const<int>{}.g() != 0) return 1;
  return use<int* const>() == 2 && use<int*>() == 1 ? 0 : 2;
}
